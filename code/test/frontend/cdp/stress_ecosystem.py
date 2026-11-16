"""Scale check of the Ecosystem window (R94): is it still usable with dozens of connections and hundreds of tables?

  py -3 code/test/frontend/cdp/stress_ecosystem.py <connections> <databases per connection> <tables per database> [--release] [--timeline]
  e.g.  12 3 25   (900 tables: every one is drawn)     50 4 40   (8,000 tables: the picture is cut down)

Fake data folders (<connection>/<database>/<table>.schema.json + .rdb, no engine needed) are made in the temp folder, put into code/data/connections.json as extra connections (the
user's file is saved first and put back byte for byte, whatever happens), and the debug app (or --release: the release exe) draws them. Printed: the cylinders, arrows and packets
drawn, the time until the picture is there, the frames a second while nothing is touched, and the milliseconds one hover and one zoom step take. Only structure is asserted -- what the
picture is made of, that a heavy one says its animation is off, that no error is shown -- because speeds depend on the machine (a 979-cylinder picture measured 177 frames a second,
18-36 ms per hover on the development PC; before R94 it was 47 frames and 80-190 ms).
"""
import json
import shutil
import struct
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cdp_lib  # noqa: E402
from cdp_lib import CONNECTIONS, Processes  # noqa: E402

N, D, T = (int(x) for x in sys.argv[1:4])
REL = "--release" in sys.argv
if REL:
    cdp_lib.APP_EXE = cdp_lib.FRONTEND / "src-tauri" / "target" / "release" / "rusql-ui.exe"
FAKE = Path(tempfile.gettempdir()) / f"rusql_eco_stress_{N}_{D}_{T}"
HEAVY_NODES, MAX_TABLES_LIMITS = 300, (40, 20, 10)   # the same numbers as lib/ecosystemLayout.ts


def make_fake():
    shutil.rmtree(FAKE, ignore_errors=True)
    rows = [b'{"_xmax":"0","id":1}', b'{"_xmax":"0","id":2}', b'{"_xmax":"5","id":3}']   # two live rows and one deleted version
    raw = b"".join(struct.pack("<I", len(r)) + r for r in rows)
    head = bytes([0, 66, 68, 82, 1, 0, 0, 0]) + struct.pack("<I", 3) + bytes(20)         # a table file: magic, version, flags 0 (plain), 3 row versions
    for c in range(N):
        (FAKE / f"stress{c}" / "_system").mkdir(parents=True)
        for d in range(D):
            db = FAKE / f"stress{c}" / f"db{d}"
            db.mkdir()
            for t in range(T):
                (db / f"table_{t}.schema.json").write_text(json.dumps({"columns": [{"name": "id", "primary_key": True}, {"name": "v"}]}), encoding="utf-8")
                (db / f"table_{t}.rdb").write_bytes(head + raw)


FPS = """new Promise(res => { let n = 0; const t0 = performance.now(); function f() { n++; if (performance.now() - t0 < 2000) requestAnimationFrame(f); else res(n / ((performance.now() - t0) / 1000)); } requestAnimationFrame(f); })"""
HOVER = """new Promise(res => { const el = document.querySelector('[data-id="%s"]'); const t0 = performance.now(); el.dispatchEvent(new MouseEvent('mouseover', {bubbles: true}));
  requestAnimationFrame(() => requestAnimationFrame(() => res(performance.now() - t0))); })"""
ZOOM = """new Promise(res => { const s = document.querySelector('.eco-zoom input'); const t0 = performance.now();
  Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(s, %d); s.dispatchEvent(new Event('input', {bubbles: true}));
  requestAnimationFrame(() => requestAnimationFrame(() => res(performance.now() - t0))); })"""


def main() -> int:
    backup = CONNECTIONS.read_bytes()
    procs = Processes()
    failed = []
    try:
        make_fake()
        conns = json.loads(backup.decode("utf-8"))
        for c in range(N):
            conns.append({"id": f"stress{c}", "name": f"stress-{c}", "host": "localhost", "port": 7878, "user": "root", "password": "root", "autoLogin": False,
                          "dataDir": str(FAKE / f"stress{c}"), "favorite": False, "lastUsed": 0})
        CONNECTIONS.write_text(json.dumps(conns, ensure_ascii=False, indent=2), encoding="utf-8")
        if not REL:
            procs.vite()
        page = procs.app()
        page.wait("document.querySelectorAll('.home-quick-btn').length === 4", 60, "the home screen")
        page.call("Emulation.setDeviceMetricsOverride", width=1320, height=990, deviceScaleFactor=1, mobile=False)
        t0 = time.time()
        page.click(".home-quick-btn", "View Topology")
        # the picture with the databases and tables (a first read that failed would leave only the connections: 1 + all of them)
        page.wait("document.querySelectorAll('.eco-node.eco-database').length > 0", 180, "the databases of the fake connections")
        drawn = time.time() - t0
        time.sleep(2)
        nodes, tables = page.count(".eco-node"), page.count(".eco-node.eco-table")
        print(f"{N}x{D}x{T}: cylinders={nodes} (tables {tables}) arrows={page.count('.eco-arrow')} packets={page.count('.dg-apacket')} time until the databases are there={drawn:.2f}s", flush=True)
        print("  frames a second, idle:", round(page.eval(FPS, await_promise=True), 1))
        ids = page.eval("[...document.querySelectorAll('.eco-node.eco-table')].slice(0, 3).map(e => e.dataset.id)")
        print("  one hover, ms:", [round(page.eval(HOVER % i, await_promise=True), 1) for i in ids])
        print("  one zoom step, ms:", [round(page.eval(ZOOM % z, await_promise=True), 1) for z in (60, 40, 80)])
        # structure
        heavy = page.count(".eco-wrap.eco-heavy") == 1
        if heavy != (nodes > HEAVY_NODES):
            failed.append(f"the picture has {nodes} cylinders but is {'' if heavy else 'not '}heavy (limit {HEAVY_NODES})")
        if heavy and not page.exists(".eco-heavy-note"):
            failed.append("a heavy picture does not say that its animation is off")
        if page.exists(".eco-error"):
            failed.append("the window shows an error: " + page.eval("document.querySelector('.eco-error').innerText")[:200])
        shown = page.count('[data-id^="c:stress0/d:db0/"].eco-table')   # the tables drawn of the first fake database
        total = N * D * T
        limit = MAX_TABLES_LIMITS[0] if total <= 1500 else MAX_TABLES_LIMITS[1] if total <= 4000 else MAX_TABLES_LIMITS[2]
        want = T if T <= limit else limit - 1
        if shown != want:
            failed.append(f"a database of {T} tables draws {shown} of them, expected {want} (limit {limit} for {total} tables)")
        page.screenshot(Path(__file__).resolve().parent / "out" / f"stress_{N}_{D}_{T}.png")
    finally:
        CONNECTIONS.write_bytes(backup)
        print("connections.json put back byte for byte:", CONNECTIONS.read_bytes() == backup)
        procs.stop()
        shutil.rmtree(FAKE, ignore_errors=True)
    for f in failed:
        print("FAIL", f)
    print("ok" if not failed else f"{len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
