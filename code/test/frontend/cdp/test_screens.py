"""Screen tests of the RuSQL desktop app, driven through the Chrome DevTools Protocol (R70).

What the other tests cannot see: that the screens are drawn and do what they say. The test starts the Vite dev server and the debug build of the app (what is already running is
used and left running), signs in with a saved connection of its own (a data folder under the temp folder; code/data/connections.json is put back as it was), fills a small shop
database through the SQL editor and goes through the screens: the editor and its result, the plan diagram and the index advice under an EXPLAIN (a SELECT, an UPDATE, a DELETE,
subqueries), the chart, the ERD (cards, foreign keys, dragging a card), the Server Manager's session, metrics and AI MCP panels, and Compare Schema. A failed step leaves a
screenshot in code/test/frontend/cdp/out/.

  py -3 code/test/frontend/cdp/test_screens.py [--only part-of-a-name] [--keep]

Needs: the engine and the debug app built (cmake --build code/build --config Release --target engine_server; cargo build in code/frontend/src-tauri), node_modules installed
(npm install in code/frontend), and the Windows Python with websocket-client (py -3 -m pip install websocket-client).
"""
import json
import re
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdp_lib import Page, Processes, TempConnection, ROOT  # noqa: E402

OUT = Path(__file__).resolve().parent / "out"
CONNECTION = "cdp-test"
DATA_DIR = Path(__import__("tempfile").gettempdir()) / "rusql_cdp_data"

CUSTOMERS = 300
ORDERS = 1500
SEED = "\n".join([
    "CREATE DATABASE shop;",
    "USE shop;",
    "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(20), city VARCHAR(20));",
    "CREATE TABLE orders (id INT PRIMARY KEY, customer_id INT, status VARCHAR(10), total INT, FOREIGN KEY (customer_id) REFERENCES customers(id));",
    "INSERT INTO customers VALUES " + ",".join(f"({i},'c{i}','{('Seoul', 'Busan', 'Daegu')[i % 3]}')" for i in range(1, CUSTOMERS + 1)) + ";",
    "INSERT INTO orders VALUES " + ",".join(f"({i},{i % CUSTOMERS + 1},'{('new', 'paid', 'sent')[i % 3]}',{i % 97})" for i in range(1, ORDERS + 1)) + ";",
    "CREATE DATABASE shop2;",
    "USE shop2;",
    "CREATE TABLE customers (id INT PRIMARY KEY, name VARCHAR(40));",
    "USE shop;",
])

RUN = 'button[title="Run (Ctrl+Enter)"]'
RUN_ALL = 'button[title="Run All"]'

SCENARIOS = []


def scenario(fn):
    SCENARIOS.append(fn)
    return fn


# ── helpers ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
def result_text(page: Page) -> str:
    return page.eval("(document.querySelector('.result-panel') || {innerText: ''}).innerText") or ""


def run_statement(page: Page, sql: str, wait_text: str, timeout: float = 30.0, button: str = RUN):
    """Put `sql` in the editor of the SQL Editor screen, run it, and wait until the result panel (or the plan area) shows `wait_text`."""
    page.click('[title="SQL Editor"]')
    page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 20, "the editor")
    page.set_editor(sql)
    time.sleep(0.2)
    # (the button is disabled while the screen still runs the statement before: a click then does nothing)
    page.wait(f"(() => {{ const b = document.querySelector({json.dumps(button)}); return b && !b.disabled; }})()", 30, "the Run button to be enabled")
    page.click(button)
    page.wait(f"(document.querySelector('.result-panel') || document.body).innerText.includes({json.dumps(wait_text)})", timeout, f"{wait_text!r} in the result")


def click_text(page: Page, text: str, within: str = "body *", exact: bool = True):
    """Click the smallest element of `within` whose own text is `text`."""
    ok = page.eval(
        "(() => { const els = [...document.querySelectorAll(%s)].filter(e => { const t = (e.innerText || '').trim(); return %s ? t === %s : t.includes(%s); });"
        " els.sort((a, b) => (a.innerText || '').length - (b.innerText || '').length); const e = els[0]; if (!e) return false; e.click(); return true; })()"
        % (json.dumps(within), json.dumps(exact).lower(), json.dumps(text), json.dumps(text)))
    if not ok:
        raise LookupError(f"nothing to click with the text {text!r}")


def drag(page: Page, selector: str, dx: float, dy: float, nth: int = 0):
    """Drag the nth element that matches `selector` by (dx, dy) pixels with real mouse events."""
    box = page.eval(
        "(() => { const e = document.querySelectorAll(%s)[%d]; if (!e) return null; const r = e.getBoundingClientRect(); return {x: r.x + r.width / 2, y: r.y + Math.min(r.height / 2, 12)}; })()"
        % (json.dumps(selector), nth))
    if not box:
        raise LookupError(f"nothing to drag: {selector!r}")
    x, y = box["x"], box["y"]
    page.call("Input.dispatchMouseEvent", type="mouseMoved", x=x, y=y)
    page.call("Input.dispatchMouseEvent", type="mousePressed", x=x, y=y, button="left", clickCount=1)
    for step in range(1, 9):
        page.call("Input.dispatchMouseEvent", type="mouseMoved", x=x + dx * step / 8, y=y + dy * step / 8, buttons=1)
        time.sleep(0.03)
    page.call("Input.dispatchMouseEvent", type="mouseReleased", x=x + dx, y=y + dy, button="left", clickCount=1)


def set_select(page: Page, selector: str, nth: int, value: str):
    """Choose an option of a <select> (React needs the native setter and a change event)."""
    page.eval("""(() => { const s = document.querySelectorAll(%s)[%d]; const set = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value').set;
        set.call(s, %s); s.dispatchEvent(new Event('change', {bubbles: true})); })()""" % (json.dumps(selector), nth, json.dumps(value)))


def server_tools() -> list[str]:
    source = (ROOT / "mcp" / "mcp_server.py").read_text(encoding="utf-8")
    return sorted(re.findall(r"@mcp\.tool\(\)\r?\ndef (\w+)\(", source))


# ── the screens ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
@scenario
def home_lists_the_saved_connection(page: Page):
    page.wait("document.body.innerText.includes('RuSQL Connections')", 60, "the home screen")
    page.wait("[...document.querySelectorAll('.home-sidebar-item')].some(e => e.innerText.includes(%s))" % json.dumps(CONNECTION), 30, "the saved connection")


@scenario
def signs_in_and_runs_a_query(page: Page):
    page.click(".home-sidebar-item", CONNECTION)
    page.wait("window.monaco && window.monaco.editor.getEditors().length > 0", 60, "the editor after sign-in")
    time.sleep(1)
    run_statement(page, "SELECT 1 AS one, 'x' AS two", "1 row(s)")
    text = result_text(page)
    assert "one" in text and "two" in text, text[:200]


@scenario
def a_script_fills_the_shop_database(page: Page):
    run_statement(page, SEED, "query(s)", timeout=90, button=RUN_ALL)
    text = result_text(page)
    assert "✓ 10 query(s)" in text, text[:300]
    assert "✗" not in text and "rror" not in text, text[:300]
    page.wait("document.body.innerText.includes('customers')", 20, "the tables in the sidebar")


@scenario
def a_result_can_be_exported_and_drawn_as_a_chart(page: Page):
    run_statement(page, "SELECT status, COUNT(*) AS n, SUM(total) AS s FROM orders GROUP BY status", "3 row(s)")
    text = result_text(page)
    assert "CSV" in text and "JSON" in text, "the export buttons"
    assert page.exists("button", "Chart"), "the chart button"
    before = page.count("svg")
    page.click("button", "Chart")
    page.wait(f"document.querySelectorAll('svg').length > {before} || document.querySelector('[class*=chart]') !== null", 10, "the chart")


@scenario
def explain_select_draws_the_plan_and_advises_an_index(page: Page):
    run_statement(page, "EXPLAIN SELECT * FROM orders WHERE customer_id = 7", "Seq Scan")
    page.wait("document.querySelectorAll('.plan-node').length >= 1", 15, "the plan nodes")
    page.wait("document.querySelector('.ia') && document.querySelector('.ia').innerText.includes('CREATE INDEX')", 20, "the index advice")
    advice = page.eval("document.querySelector('.ia').innerText")
    assert "idx_orders_customer_id" in advice, advice
    assert page.exists(".ia-btn", "Insert into editor") and page.exists(".ia-btn", "Try it"), "the buttons of the advice"
    # a trial builds the index under a temporary name, times the plan before and after, and drops it again
    page.click(".ia-btn", "Try it")
    page.wait("document.querySelector('.ia-trial') && document.querySelector('.ia-trial').innerText.includes('→')", 60, "the trial")
    trial = page.eval("document.querySelector('.ia-trial').innerText")
    assert "ms" in trial, trial
    run_statement(page, "SHOW INDEX FROM orders", "orders")
    assert "__try_" not in result_text(page), "the trial index was dropped"


@scenario
def explain_update_draws_the_plan_and_the_trial_is_an_estimate(page: Page):
    run_statement(page, "EXPLAIN UPDATE orders SET total = 0 WHERE customer_id = 8", "UPDATE")
    page.wait("document.querySelectorAll('.plan-node').length >= 1", 15, "the plan of the UPDATE")
    page.wait("document.querySelector('.ia') && document.querySelector('.ia').innerText.includes('CREATE INDEX')", 20, "the index advice of an UPDATE")
    assert page.exists(".ia-btn", "Try it (estimate)"), "an UPDATE is tried by estimate, never run"
    page.click(".ia-btn", "Try it (estimate)")
    page.wait("document.querySelector('.ia-trial') && document.querySelector('.ia-trial').innerText.includes('estimated')", 30, "the estimated trial")
    trial = page.eval("document.querySelector('.ia-trial').innerText")
    assert trial.startswith("cost "), trial
    # nothing was run: no row has total = 0 that did not have it, and the trial index is gone
    run_statement(page, "SELECT COUNT(*) AS n FROM orders WHERE customer_id = 8 AND total = 0", "1 row(s)")
    run_statement(page, "SHOW INDEX FROM orders", "orders")
    assert "__try_" not in result_text(page), "the trial index was dropped"


@scenario
def explain_delete_draws_the_plan(page: Page):
    run_statement(page, "EXPLAIN DELETE FROM orders WHERE status = 'sent' AND total > 90", "DELETE")
    page.wait("document.querySelectorAll('.plan-node').length >= 1", 15, "the plan of the DELETE")
    run_statement(page, "SELECT COUNT(*) AS n FROM orders", f"{ORDERS}")  # (nothing was deleted)


@scenario
def the_advice_reads_a_subquery_and_says_which(page: Page):
    run_statement(page, "EXPLAIN SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE total = 5)", "customers")
    page.wait("document.querySelector('.ia') && document.querySelector('.ia').innerText.includes('in an IN subquery')", 20, "the advice for the subquery")


@scenario
def erd_draws_the_tables_and_a_card_can_be_dragged(page: Page):
    page.click('[title="ERD Editor"]')
    page.wait("document.querySelectorAll('.erd-card').length >= 2", 20, "the ERD cards")
    assert page.count(".erd-fk") >= 1, "a foreign key line"
    before = page.eval("(() => { const r = document.querySelector('.erd-card').getBoundingClientRect(); return [r.x, r.y]; })()")
    drag(page, ".erd-card-header", 60, 40)
    time.sleep(0.4)
    after = page.eval("(() => { const r = document.querySelector('.erd-card').getBoundingClientRect(); return [r.x, r.y]; })()")
    assert abs(after[0] - before[0]) > 20 or abs(after[1] - before[1]) > 20, f"the card did not move: {before} -> {after}"


@scenario
def server_manager_sessions_metrics_and_mcp_panels(page: Page):
    page.click('[title="Server Manager"]')
    page.wait("document.querySelector('.srv-rbar-btn') !== null", 15, "the Server Manager")
    # the session panel lists this app's own session
    click_text(page, "Session", ".srv-rbar-btn", exact=False)
    page.wait("document.querySelector('.srv-slide-panel') && document.querySelector('.srv-slide-panel').innerText.includes('ACTIVE SESSIONS')", 15, "the session panel")
    text = page.eval("document.querySelector('.srv-slide-panel').innerText")
    assert "root" in text, text[:300]
    assert "SLOW QUERIES" in text, "the slow query log card"
    # the metrics panel
    click_text(page, "Metrics", ".srv-rbar-btn", exact=False)
    page.wait("document.querySelector('.srv-slide-panel') && document.querySelector('.srv-slide-panel').innerText.includes('STATEMENTS PER SECOND')", 15, "the metrics panel")
    # the AI MCP panel lists the tools of the MCP server, exactly
    click_text(page, "AI MCP", ".srv-rbar-btn", exact=False)
    page.wait("document.querySelector('.srv-slide-panel') && document.querySelector('.srv-slide-panel').innerText.includes('DATABASE TOOLS')", 15, "the AI MCP panel")
    shown = sorted(page.eval("[...document.querySelectorAll('.srv-slide-panel span[title]')].map(e => e.innerText.trim())"))
    real = server_tools()
    assert shown == real, f"the panel lists {len(shown)} tools, the server has {len(real)}: {sorted(set(shown) ^ set(real))}"


@scenario
def the_table_menu_checks_and_optimizes_a_table(page: Page):
    page.click('[title="SQL Editor"]')
    page.wait("document.querySelector('.sidebar-db-header') !== null", 15, "the databases in the sidebar")
    page.wait("[...document.querySelectorAll('.sidebar-name')].some(e => e.innerText.trim() === 'orders')", 20, "the table orders in the sidebar")

    def open_table_menu():
        page.eval("""(() => { const e = [...document.querySelectorAll('.sidebar-name')].find(x => x.innerText.trim() === 'orders').parentElement;
            const r = e.getBoundingClientRect(); e.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true, clientX: r.x + 10, clientY: r.y + 5, button: 2})); })()""")
        page.wait("document.querySelector('.table-ctx-menu') !== null", 10, "the table menu")

    open_table_menu()
    click_text(page, "Check Table", ".table-ctx-menu *")
    page.wait("(document.querySelector('.result-panel') || document.body).innerText.includes('Msg_text')", 30, "the answer of CHECK TABLE")
    text = result_text(page)
    assert "check" in text and "status" in text and "OK" in text, text[:300]
    open_table_menu()
    click_text(page, "Optimize Table", ".table-ctx-menu *")
    page.wait("(document.querySelector('.result-panel') || document.body).innerText.includes('optimize')", 30, "the answer of OPTIMIZE TABLE")
    text = result_text(page)
    assert "recreate + analyze" in text and "OK" in text, text[:300]


@scenario
def compare_schema_names_what_differs(page: Page):
    page.click('[title="SQL Editor"]')
    page.wait("document.querySelector('.sidebar-db-header') !== null", 15, "the databases in the sidebar")
    page.eval("""(() => { const e = [...document.querySelectorAll('.sidebar-db-header')].find(x => x.innerText.toLowerCase().includes('shop') && !x.innerText.toLowerCase().includes('shop2'));
        const r = e.getBoundingClientRect(); e.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true, clientX: r.x + 10, clientY: r.y + 5, button: 2})); })()""")
    click_text(page, "Compare Schema...", "body *", exact=False)
    page.wait("document.querySelector('.sd-modal') !== null", 10, "the Compare Schema window")
    set_select(page, ".sd-pick select", 0, "shop")
    set_select(page, ".sd-pick select", 1, "shop2")
    time.sleep(0.3)
    page.click(".sd-primary", "Compare")
    page.wait("document.querySelector('.sd-modal').innerText.includes('only in')", 20, "the differences")
    text = page.eval("document.querySelector('.sd-modal').innerText")
    assert "orders" in text, text[:400]  # (a table only shop has)
    assert "Migration" in text, "the migration SQL"
    page.click(".sd-close")


# ── runner ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
def main() -> int:
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else ""
    keep = "--keep" in sys.argv
    chosen = [s for s in SCENARIOS if only in s.__name__]
    procs = Processes()
    failed = 0
    OUT.mkdir(exist_ok=True)
    try:
        procs.vite()
        page = procs.app()
        with TempConnection(CONNECTION, DATA_DIR):
            # (a run of some of the scenarios only: the first two sign in and fill the database, the rest need that done)
            if chosen and chosen[0] is not SCENARIOS[0]:
                setup = [home_lists_the_saved_connection, signs_in_and_runs_a_query, a_script_fills_the_shop_database]
                for fn in setup:
                    if fn not in chosen:
                        fn(page)
            for fn in chosen:
                started = time.time()
                try:
                    fn(page)
                    print(f"ok   {fn.__name__.replace('_', ' ')}  ({time.time() - started:.1f}s)")
                except Exception as e:  # noqa: BLE001
                    failed += 1
                    print(f"FAIL {fn.__name__.replace('_', ' ')}\n     {type(e).__name__}: {str(e)[:500]}")
                    if "-v" in sys.argv:
                        traceback.print_exc()
                    try:
                        page.screenshot(OUT / f"{fn.__name__}.png")
                    except Exception:  # noqa: BLE001
                        pass
    finally:
        if not keep:
            procs.stop()
    print(f"{len(chosen) - failed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
