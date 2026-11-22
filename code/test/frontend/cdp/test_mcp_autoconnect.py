"""The "Auto-connect Claude Desktop" button of an INSTALLED RuSQL, without touching the real Claude Desktop config:
    py -3 code/test/frontend/cdp/test_mcp_autoconnect.py <folder with rusql-ui.exe, engine/ and mcp/>

The app runs with an APPDATA / LOCALAPPDATA of its own, `setup_mcp_config` is invoked, the config it writes is read back and checked, and the command it wrote is started the way Claude Desktop
would start it (stdio) to list the tools. It found that the button could write a Python WITHOUT the `mcp` package into the config: `import mcp` is true when the current folder has a folder
called mcp, and the install folder has one (mcp/mcp_server.py).
"""
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cdp_lib import Page, port_open, wait_for  # noqa: E402

sys.stdout.reconfigure(encoding="utf-8")
install = Path(sys.argv[1]).resolve()
work = Path(tempfile.mkdtemp(prefix="rusql_autoconn_"))
appdata, local = work / "appdata", work / "local"
appdata.mkdir()
local.mkdir()
env = dict(os.environ, APPDATA=str(appdata), LOCALAPPDATA=str(local), WEBVIEW2_USER_DATA_FOLDER=str(work / "wv2"),
           WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS="--remote-debugging-port=9338")
env.pop("RUSQL_DATA_DIR", None)
failures = []


def check(ok, what, detail=""):
    print(("PASS  " if ok else "FAIL  ") + what + (("   [%s]" % detail) if detail and not ok else ""))
    if not ok:
        failures.append(what)


if port_open(9338):
    print("port 9338 is in use")
    sys.exit(2)
app = subprocess.Popen([str(install / "rusql-ui.exe")], env=env, cwd=str(install), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    wait_for(lambda: port_open(9338), 60, what="DevTools port")
    time.sleep(2)
    page = Page(9338)
    try:
        page.eval("window.__TAURI_INTERNALS__.invoke('setup_mcp_config', %s)" % json.dumps({"host": "127.0.0.1", "port": 7878, "user": "root", "password": "root"}), await_promise=True)
    except Exception as e:  # noqa: BLE001
        check(False, "setup_mcp_config answers", str(e)[:300])
    cfg_path = appdata / "Claude" / "claude_desktop_config.json"
    check(cfg_path.is_file(), "the config file is written under %APPDATA%\\Claude", str(cfg_path))
    if cfg_path.is_file():
        entry = json.loads(cfg_path.read_text(encoding="utf-8"))["mcpServers"]["RuSQL"]
        check(Path(entry["command"]).is_file(), "the Python it names exists", entry["command"])
        check(entry["args"][0] == "-u" and Path(entry["args"][1]) == install / "mcp" / "mcp_server.py", "it starts the mcp_server.py of the INSTALL folder", str(entry["args"]))
        e = entry["env"]
        check(e["RUSQL_HOST"] == "127.0.0.1" and e["RUSQL_PORT"] == "7878" and e["RUSQL_USER"] == "root", "host, port and user are passed in env", str(e))
        check(Path(e["RUSQL_APP_PATH"]) == install / "rusql-ui.exe", "RUSQL_APP_PATH is the installed app (launch_app can start it)", e["RUSQL_APP_PATH"])
        check(Path(e["RUSQL_DATA_DIR"]) == appdata / "RuSQL" / "data", "RUSQL_DATA_DIR is the app's own data folder", e["RUSQL_DATA_DIR"])
        check(len(entry["alwaysAllow"]) >= 20, "the always-allowed list is there", str(len(entry["alwaysAllow"])))

        from mcp import ClientSession, StdioServerParameters
        from mcp.client.stdio import stdio_client

        async def go():
            params = StdioServerParameters(command=entry["command"], args=entry["args"], env=dict(os.environ, **entry["env"]))
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    return sorted(t.name for t in (await session.list_tools()).tools)

        try:
            names = asyncio.run(go())
            check(len(names) == 37, "started from that config, the server lists 37 tools", str(len(names)))
        except BaseException as ex:  # noqa: BLE001  (an ExceptionGroup when the server dies at start, e.g. No module named 'mcp')
            check(False, "started from that config, the server lists 37 tools", "%s: %s" % (type(ex).__name__, str(ex)[:200]))
finally:
    subprocess.run(["taskkill", "/PID", str(app.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)
    if app.poll() is None:
        subprocess.run(["taskkill", "/F", "/T", "/PID", str(app.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("\nALL PASS" if not failures else "\n%d FAILED: %s" % (len(failures), "; ".join(failures)))
sys.exit(1 if failures else 0)
