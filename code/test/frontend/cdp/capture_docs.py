"""The screenshots of the deliverables (docs/deliverables/.../capture and the pictures of D06 / D08 / the integrated document), taken from the running app: 1320 x 990.

Starts the Vite dev server and the debug build of the app the way test_screens.py does, signs in with a connection of its own, fills the small shop database and takes one picture of
every screen the documents show, into the folder given (default: code/test/frontend/cdp/out/docs). A screen that cannot be reached is named and left out.

  py -3 code/test/frontend/cdp/capture_docs.py [folder]
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_screens as ts  # noqa: E402
from cdp_lib import Processes, TempConnection  # noqa: E402

OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent / "out" / "docs"
ONLY = set(sys.argv[2].split(",")) if len(sys.argv) > 2 else set()  # (e.g. "07,08": only those pictures)
ts.CONNECTION = "RuSQL Local"  # (the name the home screen shows)

JOIN = ("SELECT c.name, c.city, COUNT(*) AS orders, SUM(o.total) AS total FROM customers c JOIN orders o ON o.customer_id = c.id "
        "GROUP BY c.id, c.name, c.city ORDER BY total DESC LIMIT 10")


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    procs = Processes()
    missed = []
    try:
        procs.vite()
        page = procs.app()
        page.call("Emulation.setDeviceMetricsOverride", width=1320, height=990, deviceScaleFactor=1, mobile=False)

        def shot(name: str, pause: float = 0.8):
            time.sleep(pause)
            page.screenshot(OUT / name)
            print("shot", name)

        def step(name: str, fn):
            if ONLY and name not in ONLY:
                return
            try:
                fn()
            except Exception as e:  # noqa: BLE001
                missed.append(name)
                print("MISSED", name, type(e).__name__, str(e)[:200])
                try:
                    page.screenshot(OUT / f"missed_{name}.png")
                    page.eval("window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}))")
                except Exception:  # noqa: BLE001
                    pass

        def panel(title: str, name: str, wait: str, pause: float = 1.5):
            page.click(f'[title="{title}"]')
            page.wait(wait, 20, name)
            shot(name, pause)

        with TempConnection(ts.CONNECTION, ts.DATA_DIR):
            ts.home_lists_the_saved_connection(page)
            if not ONLY or "01" in ONLY:
                shot("01_홈화면.png", 1.5)

            def new_connection():
                page.click(".home-quick-btn", "New Connection")
                page.wait("document.querySelector('input') !== null", 10, "the form")
                shot("02_새연결추가.png")
                page.eval("window.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'}))")
                try:
                    ts.click_text(page, "Cancel")
                except LookupError:
                    pass
            step("02", new_connection)

            ts.signs_in_and_runs_a_query(page)
            ts.a_script_fills_the_shop_database(page)
            ts.drag(page, ".divider", 0, -300)  # (the results get more room than the 260 pixels they start with)
            time.sleep(0.5)

            def editor():
                ts.run_statement(page, JOIN, "10 row(s)")
                shot("04_SQL편집기.png")
            step("04", editor)

            def chart():
                ts.run_statement(page, "SELECT status, COUNT(*) AS n, SUM(total) AS s FROM orders GROUP BY status", "3 row(s)")
                page.click("button", "Chart")
                time.sleep(1.0)
                shot("05_결과차트.png")
            step("05", chart)

            def plan():
                ts.run_statement(page, "EXPLAIN SELECT * FROM orders WHERE customer_id = 7", "Seq Scan")
                page.wait("document.querySelectorAll('.plan-node').length >= 1", 15, "the plan nodes")
                page.wait("document.querySelector('.ia') && document.querySelector('.ia').innerText.includes('CREATE INDEX')", 20, "the index advice")
                shot("06_실행계획과_인덱스제안.png")
            step("06", plan)

            def analyze():
                ts.run_statement(page, "EXPLAIN ANALYZE SELECT o.status, COUNT(*) FROM orders o JOIN customers c ON c.id = o.customer_id WHERE c.city = 'Seoul' GROUP BY o.status", "QUERY PLAN")
                shot("07_EXPLAIN_ANALYZE.png")
            step("07", analyze)

            def journey():
                ts.run_statement(page, "SELECT * FROM orders WHERE customer_id = 7;", "row(s)")
                page.eval("(() => { const e = window.monaco.editor.getEditors()[0]; e.focus(); e.setSelection(e.getModel().getFullModelRange()); })()")  # (the journey takes the selection)
                time.sleep(0.3)
                page.click('button[title^="Journey"]')
                page.wait("document.querySelector('.jn-modal') !== null", 30, "the journey window")
                if page.exists(".jn-primary"):
                    page.click(".jn-primary")
                time.sleep(4.0)
                shot("08_문장의여행.png", 1.0)
                page.eval("(document.querySelector('.modal-overlay') || {click() {}}).click()")
            step("08", journey)

            def compare():
                page.click('[title="SQL Editor"]')
                page.wait("document.querySelector('.sidebar-db-header') !== null", 15, "the databases")
                page.eval("""(() => { const e = [...document.querySelectorAll('.sidebar-db-header')].find(x => x.innerText.toLowerCase().includes('shop') && !x.innerText.toLowerCase().includes('shop2'));
                    const r = e.getBoundingClientRect(); e.dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true, clientX: r.x + 10, clientY: r.y + 5, button: 2})); })()""")
                ts.click_text(page, "Compare Schema...", "body *", exact=False)
                page.wait("document.querySelector('.sd-modal') !== null", 10, "the Compare Schema window")
                ts.set_select(page, ".sd-pick select", 0, "shop")
                ts.set_select(page, ".sd-pick select", 1, "shop2")
                time.sleep(0.3)
                page.click(".sd-primary", "Compare")
                page.wait("document.querySelector('.sd-modal').innerText.includes('only in')", 20, "the differences")
                shot("09_스키마비교.png")
                page.click(".sd-close")
            step("09", compare)

            def erd():
                page.click('[title="ERD Editor"]')
                page.wait("document.querySelectorAll('.erd-card').length >= 2", 20, "the ERD cards")
                shot("10_ERD_Editor.png", 1.5)
            step("10", erd)

            def server():
                page.click('[title="Server Manager"]')
                page.wait("document.querySelector('.srv-rbar-btn') !== null", 15, "the Server Manager")
                shot("11_서버관리.png", 1.5)
            step("11", server)
            def sessions():
                # (a transaction left open, so that the panel has something to show: the transaction with the row locks it holds)
                ts.run_statement(page, "BEGIN;\nUPDATE orders SET status = 'paid' WHERE id <= 3;", "2 query(s)", button=ts.RUN_ALL)
                page.click('[title="Server Manager"]')
                page.wait("document.querySelector('.srv-rbar-btn') !== null", 15, "the Server Manager")
                panel("Sessions", "12_세션_트랜잭션_락.png", "(document.querySelector('.srv-slide-panel') || {innerText: ''}).innerText.includes('row lock')")
            step("12", sessions)
            step("13", lambda: panel("Metrics", "13_서버지표_Metrics.png", "(document.querySelector('.srv-slide-panel') || {innerText: ''}).innerText.includes('STATEMENTS PER SECOND')", 4.0))
            step("14", lambda: panel("MCP Agent", "14_AI_MCP.png", "(document.querySelector('.srv-slide-panel') || {innerText: ''}).innerText.includes('DATABASE TOOLS')"))
            step("15", lambda: panel("Benchmark", "15_Bench.png", "document.querySelector('.srv-slide-panel') !== null"))
            step("16", lambda: panel("MySQL Connection", "16_MySQL접속안내.png", "document.querySelector('.srv-slide-panel') !== null"))
            step("17", lambda: panel("CLI Guide", "17_CLI접속안내.png", "document.querySelector('.srv-slide-panel') !== null"))

            def page_view(title: str, name: str, pause: float, stop_at=None):
                # (the documents are in Korean: the page is switched to 한국어; a scripted page is moved to step `stop_at` (negative: from the end) and plays on from there)
                def run():
                    page.click(f'[title="{title}"]')
                    time.sleep(pause)
                    page.eval("(() => { const b = [...document.querySelectorAll('button')].find(x => x.innerText.trim() === '한국어'); if (b && b.getAttribute('aria-pressed') !== 'true') b.click(); })()")
                    time.sleep(0.8)
                    if stop_at is not None:
                        page.eval(f"""(() => {{ const t = [...document.querySelectorAll('.pk-track i')]; const i = {stop_at} < 0 ? t.length + {stop_at} : {stop_at}; t[i].click();
                            const p = document.querySelector('.pk-play-btn'); if (p && p.innerText.trim() === '▶') p.click(); }})()""")
                        time.sleep(0.9)
                    shot(name, 0.3)
                return run
            step("18", page_view("Diagram", "18_Diagram.png", 3.0))
            step("19", page_view("B+Tree", "19_BTree.png", 2.0, -1))
            step("20", page_view("MVCC", "20_MVCC.png", 2.0, 3))
            step("21", page_view("MySQL Compatibility", "21_MySQL호환성.png", 2.0))
    finally:
        procs.stop()
    print("missed:", missed or "none")
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
