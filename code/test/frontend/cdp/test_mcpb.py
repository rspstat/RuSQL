"""The "Add to Claude Desktop (extension)" button of an INSTALLED RuSQL, without touching the real Claude Desktop:
    py -3 code/test/frontend/cdp/test_mcpb.py <folder with rusql-ui.exe, engine/, python/ and mcp/> [--no-system-python]

The app runs with an APPDATA of its own, `setup_mcp_extension` is invoked with open=false (so no install window of the real Claude Desktop opens), and the .mcpb it writes is checked:
a zip with manifest.json, icon.png and the server file the manifest names. Then it is unpacked the way Claude Desktop does (the extension folder replaces ${__dirname}) and the command
of the manifest is started over stdio, which must list the 37 tools. Why the extension exists: a newly installed Microsoft Store Claude Desktop does not always read
claude_desktop_config.json (github.com/anthropics/claude-code/issues/26073), the extension goes in through Claude Desktop's own install window instead.
"""
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import time
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdp_lib import Page, port_open, wait_for  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")
install = Path(sys.argv[1]).resolve()
work = Path(tempfile.mkdtemp(prefix="rusql_mcpb_"))
appdata, local = work / "appdata", work / "local"
appdata.mkdir()
local.mkdir()
env = dict(os.environ, APPDATA=str(appdata), LOCALAPPDATA=str(local), WEBVIEW2_USER_DATA_FOLDER=str(work / "wv2"),
           WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9339")
env.pop("RUSQL_DATA_DIR", None)
if "--no-system-python" in sys.argv:  # the PC of the exhibition may have no Python: take every folder with a Python out of the PATH the app sees
    env["PATH"] = os.pathsep.join(p for p in env.get("PATH", "").split(os.pathsep) if "python" not in p.lower() and "msys64" not in p.lower())
failures = []


def check(ok, what, detail=""):
    print(("PASS  " if ok else "FAIL  ") + what + (("   [%s]" % detail) if detail and not ok else ""))
    if not ok:
        failures.append(what)


if port_open(9339):
    print("port 9339 is in use")
    sys.exit(2)
app = subprocess.Popen([str(install / "rusql-ui.exe")], env=env, cwd=str(install), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    wait_for(lambda: port_open(9339), 60, what="DevTools port")
    time.sleep(2)
    page = Page(9339)
    msg = ""
    try:
        msg = page.eval("window.__TAURI_INTERNALS__.invoke('setup_mcp_extension', %s)" % json.dumps({"host": "127.0.0.1", "port": 7878, "user": "root", "password": "root", "open": False}), await_promise=True)
    except Exception as e:  # noqa: BLE001
        check(False, "setup_mcp_extension answers", str(e)[:300])
    mcpb = appdata / "RuSQL" / "RuSQL.mcpb"
    check(mcpb.is_file(), "the extension file is written to %APPDATA%\\RuSQL\\RuSQL.mcpb", str(mcpb))
    check(str(mcpb) in msg, "the answer names the file", msg[:200])
    if mcpb.is_file():
        check(zipfile.is_zipfile(mcpb), "it is a zip file")
        with zipfile.ZipFile(mcpb) as z:
            names = sorted(z.namelist())
            check(names == ["icon.png", "manifest.json", "server/mcp_server.py"], "it holds manifest.json, icon.png and server/mcp_server.py", str(names))
            manifest = json.loads(z.read("manifest.json").decode("utf-8"))
            check(z.read("icon.png")[:4] == b"\x89PNG", "the icon is a PNG")
            ext = work / "extension"  # (Claude Desktop unpacks an extension into a folder of its own)
            z.extractall(ext)
        check(manifest["name"] == "rusql" and manifest["manifest_version"] in ("0.2", "0.3") and manifest["author"]["name"] == "rspstat", "name rusql, a known manifest version, author rspstat", str(manifest)[:200])
        server = manifest["server"]
        check((ext / server["entry_point"]).is_file(), "the entry point named in the manifest is in the file", server["entry_point"])
        cfg = server["mcp_config"]
        check(Path(cfg["command"]).is_file(), "the Python it names exists", cfg["command"])
        bundled = install / "python" / "python.exe"
        if bundled.is_file() and bundled.stat().st_size > 10_000:  # the installer carries a Python with mcp: it is the one to name, and it writes no .pyc into the install folder
            check(Path(cfg["command"]) == bundled and cfg["args"][0] == "-B", "it names the Python of the INSTALL folder (python\\python.exe -B)", str(cfg["command"]) + " " + str(cfg["args"]))
        check(cfg["args"][-2] == "-u" and cfg["args"][-1] == "${__dirname}/server/mcp_server.py", "the server file is addressed through ${__dirname}", str(cfg["args"]))
        e = cfg["env"]
        check(e["RUSQL_HOST"] == "127.0.0.1" and e["RUSQL_PORT"] == "7878" and e["RUSQL_USER"] == "root", "host, port and user are passed in env", str(e))
        check(Path(e["RUSQL_APP_PATH"]) == install / "rusql-ui.exe", "RUSQL_APP_PATH is the installed app (launch_app can start it)", e["RUSQL_APP_PATH"])
        check(Path(e["RUSQL_DATA_DIR"]) == appdata / "RuSQL" / "data", "RUSQL_DATA_DIR is the app's own data folder", e["RUSQL_DATA_DIR"])
        check(not (appdata / "Claude").exists() and not (local / "Packages").exists(), "the config-file route was not touched (no Claude config folders written)", str(sorted(p.name for p in local.iterdir())))
        if bundled.is_file():
            check(not any((install / "python").rglob("*.pyc")), "no .pyc file was written into the install folder's python\\", str(sum(1 for _ in (install / "python").rglob("*.pyc"))))

        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client

        async def go():
            args = [a.replace("${__dirname}", str(ext)) for a in cfg["args"]]  # what Claude Desktop does before it starts the command
            params = StdioServerParameters(command=cfg["command"], args=args, env=dict(os.environ, **cfg["env"]))
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    return sorted(t.name for t in (await session.list_tools()).tools)

        try:
            names = asyncio.run(go())
            check(len(names) == 37, "unpacked and started the way Claude Desktop does, the server lists 37 tools", str(len(names)))
        except BaseException as ex:  # noqa: BLE001  (an ExceptionGroup when the server dies at start, e.g. No module named 'mcp')
            check(False, "unpacked and started the way Claude Desktop does, the server lists 37 tools", "%s: %s" % (type(ex).__name__, str(ex)[:200]))
finally:
    subprocess.run(["taskkill", "/PID", str(app.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    if app.poll() is None:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(app.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("\nALL PASS" if not failures else "\n%d FAILED: %s" % (len(failures), "; ".join(failures)))
sys.exit(1 if failures else 0)
