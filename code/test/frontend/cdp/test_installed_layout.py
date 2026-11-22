"""Does the app work from where an installer puts it, with no code/ tree around it? (R92: the engine travels with the installer.)

An installed RuSQL is one folder: the app exe, engine/ (engine_server.exe and the three Visual C++ runtime DLLs it imports) and mcp/. This starts the exe of such a folder with
an APPDATA of its own (so nothing of the real one is touched), a WebView2 profile of its own and the DevTools port open, then asks the app, through its own commands, to
sign in and run SQL, and checks

  * the engine that was started is the engine/engine_server.exe of that folder (not one of a code/ tree),
  * the Visual C++ runtime DLLs it loaded are the copies next to it (a PC that never installed the redistributable has none anywhere else),
  * the data folder is %APPDATA%\\RuSQL\\data (an installed app has no code/data; Program Files cannot be written),
  * the app's background thread, which needs that folder, is running (app_instances.json is written),
  * SQL runs, and the table is made in the data folder of the connection.

Use the folder an installer makes, or one made by extracting the setup file (the NSIS setup of `npm run tauri build` opens with Bandizip / 7-Zip):
    py -3 code/test/frontend/cdp/test_installed_layout.py <folder with rusql-ui.exe and engine/>
With --tree the folder is the one of an exe INSIDE the source tree (code/frontend/src-tauri/target/release): there the app must keep working the old way -- the engine of
code/build, not the copy that tauri-build puts in target/release/engine -- (the data folder is moved to a temporary one with RUSQL_DATA_DIR so that nothing of the real code/data is used).
The Windows Python is needed (pip install websocket-client). Exit code 0 when everything holds.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdp_lib import Page, port_open, wait_for  # noqa: E402

RUNTIME = ["msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"]


def powershell(script: str):
    out = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True, encoding="utf-8").stdout.strip()
    return json.loads(out) if out else None


def as_list(value):
    return value if isinstance(value, list) else ([] if value is None else [value])


# the titles of the console windows that are on screen (a console program started without CREATE_NO_WINDOW gets one whose title is its exe path; closing it kills the program)
VISIBLE_CONSOLE_TITLES = r'''
Add-Type -TypeDefinition @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class Wins {
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    public static List<string> Console() {
        var r = new List<string>();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var c = new StringBuilder(256); GetClassName(h, c, 256);
            if (c.ToString() != "ConsoleWindowClass" && c.ToString() != "CASCADIA_HOSTING_WINDOW_CLASS") return true;
            var t = new StringBuilder(512); GetWindowText(h, t, 512);
            r.Add(t.ToString());
            return true;
        }, IntPtr.Zero);
        return r;
    }
}
"@
ConvertTo-Json -InputObject @([Wins]::Console())
'''


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("install_dir", help="the folder with the app exe, engine/ and mcp/")
    parser.add_argument("--port", type=int, default=9333, help="DevTools port for the web view")
    parser.add_argument("--tree", action="store_true", help="the exe is inside the source tree: check that it still uses the engine of code/build")
    args = parser.parse_args()

    install = Path(args.install_dir).resolve()
    exe = next((p for p in (install / "rusql-ui.exe", install / "RuSQL.exe") if p.is_file()), None)
    if exe is None:
        print(f"no rusql-ui.exe / RuSQL.exe in {install}")
        return 2
    engine = install / "engine" / "engine_server.exe"
    tree_engines = [Path(__file__).resolve().parents[3] / "build" / "backend" / "server" / kind / "engine_server.exe" for kind in ("Release", "Debug")]
    failures: list[str] = []

    def check(ok: bool, what: str, detail: str = ""):
        print(("PASS  " if ok else "FAIL  ") + what + (f"   [{detail}]" if detail and not ok else ""))
        if not ok:
            failures.append(what)

    if not args.tree:
        check(engine.is_file() and engine.stat().st_size > (1 << 20), "the folder carries the engine (engine/engine_server.exe)", str(engine))
        for name in RUNTIME:
            check((install / "engine" / name).is_file(), f"... and {name} next to it")
        check((install / "mcp" / "mcp_server.py").is_file(), "the folder carries mcp/mcp_server.py")
        check((install / "python" / "python.exe").is_file() and (install / "python" / "Lib" / "site-packages" / "mcp").is_dir(), "the folder carries its own Python with the mcp package (python/)")

    work = Path(tempfile.mkdtemp(prefix="rusql_installed_"))
    appdata = work / "appdata"
    data_root = appdata / "RuSQL" / "data"
    conn_dir = work / "conn1"
    real_connections = Path(__file__).resolve().parents[3] / "data" / "connections.json"
    real_before = real_connections.read_bytes() if real_connections.exists() else None
    appdata.mkdir(parents=True)
    env = dict(os.environ, APPDATA=str(appdata), WEBVIEW2_USER_DATA_FOLDER=str(work / "webview2"),
               WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f"--remote-debugging-port={args.port}")
    env.pop("RUSQL_DATA_DIR", None)
    if args.tree:
        data_root = work / "tree_data"
        env["RUSQL_DATA_DIR"] = str(data_root)
    if port_open(args.port):
        print(f"port {args.port} is in use")
        return 2
    app = subprocess.Popen([str(exe)], env=env, cwd=str(install), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for(lambda: port_open(args.port), 60, what="the app's DevTools port")
        time.sleep(2)
        page = Page(args.port)

        def invoke(command: str, **arguments):
            return page.eval("window.__TAURI_INTERNALS__.invoke(%s, %s)" % (json.dumps(command), json.dumps(arguments)), await_promise=True)

        shown = invoke("get_app_data_dir")
        check(Path(shown).resolve() == data_root.resolve(), "the data folder is " + ("RUSQL_DATA_DIR" if args.tree else "%APPDATA%\\RuSQL\\data"), shown)
        check(data_root.is_dir(), "... and the app made it")

        signed_in = invoke("authenticate", user="root", password="root", dataDir=str(conn_dir), bufferPoolSize=64)
        check(signed_in is True, "the app signs in (it starts its engine)", str(signed_in))

        engines = as_list(powershell("Get-CimInstance Win32_Process -Filter \"Name='engine_server.exe'\" | Select-Object ProcessId,ParentProcessId,ExecutablePath | ConvertTo-Json"))
        mine = [e for e in engines if e.get("ParentProcessId") == app.pid]
        check(len(mine) == 1, "one engine process is a child of the app", str(engines))
        engine_pid = mine[0]["ProcessId"] if mine else None
        titles = as_list(powershell(VISIBLE_CONSOLE_TITLES))
        check(not any("engine_server" in t for t in titles), "the engine has no console window of its own", str(titles))
        if mine:
            started = Path(mine[0]["ExecutablePath"]).resolve()
            if args.tree:
                check(started in [e.resolve() for e in tree_engines], "the engine is the one of code/build (not the copy next to the exe)", str(started))
            else:
                check(started == engine.resolve(), "the engine is the one of this folder", str(started))
            modules = [] if args.tree else as_list(powershell("(Get-Process -Id %d).Modules | Where-Object { $_.ModuleName -in 'msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll' } | "
                                         "Select-Object ModuleName,FileName | ConvertTo-Json" % mine[0]["ProcessId"]))
            for name in ([] if args.tree else RUNTIME):
                found = [m for m in modules if m["ModuleName"].lower() == name]
                check(bool(found) and Path(found[0]["FileName"]).resolve() == (install / "engine" / name).resolve(), f"the engine loaded its own {name}",
                      found[0]["FileName"] if found else "not loaded")

        answer = json.dumps(invoke("execute_query", query="SELECT 1 + 1 AS two"), ensure_ascii=False)
        check('"two"' in answer and "2" in answer, "SELECT 1 + 1 gives 2", answer[:200])
        invoke("execute_query", query="CREATE DATABASE shop")
        invoke("execute_query", query="USE shop")
        invoke("execute_query", query="CREATE TABLE t (id INT PRIMARY KEY, v VARCHAR(10))")
        invoke("execute_query", query="INSERT INTO t VALUES (1, 'a'), (2, 'b')")
        answer = json.dumps(invoke("execute_query", query="SELECT COUNT(*) AS n FROM t"), ensure_ascii=False)
        check('"n"' in answer and "2" in answer, "a table is made and read", answer[:200])
        check(any(conn_dir.rglob("t*")) or any(conn_dir.rglob("*")), "the data of the connection is in its data folder", str(conn_dir))

        invoke("save_connections", connections=[{"id": "1", "name": "t", "host": "localhost", "port": 7878, "user": "root", "password": "root", "autoLogin": False,
                                                 "dataDir": str(conn_dir), "favorite": False, "lastUsed": 0}])
        check((data_root / "connections.json").is_file(), "the connection list is saved under %APPDATA%\\RuSQL\\data")
        wait_for(lambda: (data_root / "app_instances.json").is_file(), 20, what="the instance heartbeat in app_instances.json")
        check(True, "the background thread runs (app_instances.json is written)")
        real_after = real_connections.read_bytes() if real_connections.exists() else None
        check(real_before == real_after, "the code/ tree's own data/connections.json was not touched")

        # closing the window must stop the engine the app started: a left-over engine_server.exe keeps its file locked, so the next install fails with "Error opening file for writing"
        if engine_pid:
            subprocess.run(["taskkill", "/PID", str(app.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # a plain close (no /F, no /T), like the window's X
            wait_for(lambda: app.poll() is not None, 30, what="the app to close")
            time.sleep(1)
            left = [e for e in as_list(powershell("Get-CimInstance Win32_Process -Filter \"Name='engine_server.exe'\" | Select-Object ProcessId,ExecutablePath | ConvertTo-Json")) if e.get("ProcessId") == engine_pid]
            check(not left, "closing the app stops the engine it started", str(left))
            if left:
                subprocess.run(["taskkill", "/F", "/PID", str(engine_pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # (exact PID of the one we saw)
    except Exception as e:  # (anything unexpected is a failure with its text)
        check(False, "no exception", f"{type(e).__name__}: {e}")
    finally:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(app.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1)
        shutil.rmtree(work, ignore_errors=True)
    print(("\nALL PASS" if not failures else f"\n{len(failures)} FAILED: " + "; ".join(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
