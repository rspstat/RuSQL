"""Drive the RuSQL desktop app (Tauri / WebView2) through the Chrome DevTools Protocol, for the screen tests of this folder.

The app is started with WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=<port>, which makes its web view answer the DevTools protocol on that port; this file talks to it
over a websocket (pip install websocket-client). Everything the screens show is then readable and clickable from a script: `page.eval(js)` runs JavaScript in the page, `page.wait(js)`
waits until an expression is true, `page.click(...)` clicks what a selector or a text names, `page.screenshot(path)` takes a picture.

Run it with the Windows Python (the msys one has no websocket-client): py -3 code/test/frontend/cdp/test_screens.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

try:
    import websocket  # websocket-client
except ImportError:  # pragma: no cover
    sys.exit("websocket-client is missing: py -3 -m pip install websocket-client")

ROOT = Path(__file__).resolve().parents[3]          # code/
FRONTEND = ROOT / "frontend"
APP_EXE = FRONTEND / "src-tauri" / "target" / "debug" / "rusql-ui.exe"
CONNECTIONS = ROOT / "data" / "connections.json"
VITE_PORT = 1420


def port_open(port: int, host: str = "localhost") -> bool:
    """Something listens on the port (`localhost` is tried on both IPv4 and IPv6: the Vite dev server may listen on either)."""
    try:
        with socket.create_connection((host, port), timeout=0.5):
            return True
    except OSError:
        return False


def wait_for(predicate, timeout: float = 30.0, interval: float = 0.25, what: str = "the condition"):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            last = predicate()
            if last:
                return last
        except Exception as e:  # (the page may be between two screens)
            last = e
        time.sleep(interval)
    raise TimeoutError(f"timed out after {timeout:.0f}s waiting for {what} (last: {last!r})")


class Page:
    """One page of the app's web view."""

    def __init__(self, port: int = 9222):
        targets = json.loads(urllib.request.urlopen(f"http://127.0.0.1:{port}/json", timeout=5).read())
        page = next(t for t in targets if t.get("type") == "page")
        self.ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=60, suppress_origin=True)
        self._id = 0

    def call(self, method: str, **params):
        self._id += 1
        self.ws.send(json.dumps({"id": self._id, "method": method, "params": params}))
        while True:
            message = json.loads(self.ws.recv())
            if message.get("id") == self._id:
                if "error" in message:
                    raise RuntimeError(f"{method}: {message['error']}")
                return message.get("result", {})

    def eval(self, expression: str, await_promise: bool = False):
        result = self.call("Runtime.evaluate", expression=expression, returnByValue=True, awaitPromise=await_promise)
        if "exceptionDetails" in result:
            raise RuntimeError("JavaScript error: " + json.dumps(result["exceptionDetails"].get("exception", {}).get("description", ""))[:300])
        return result.get("result", {}).get("value")

    def wait(self, expression: str, timeout: float = 30.0, what: str = ""):
        return wait_for(lambda: self.eval(expression), timeout, what=what or expression[:80])

    def screenshot(self, path: Path):
        import base64
        data = self.call("Page.captureScreenshot", format="png")["data"]
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        Path(path).write_bytes(base64.b64decode(data))

    # ── the page ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def text(self) -> str:
        return self.eval("document.body.innerText") or ""

    def count(self, selector: str) -> int:
        return int(self.eval(f"document.querySelectorAll({json.dumps(selector)}).length") or 0)

    def click(self, selector: str, text: str = "", nth: int = 0):
        """Click the nth element that matches `selector` (and holds `text`, when given)."""
        ok = self.eval(
            "(() => { const els = [...document.querySelectorAll(%s)].filter(e => !%s || (e.innerText || e.textContent || '').includes(%s));"
            " const e = els[%d]; if (!e) return false; e.scrollIntoView({block: 'center'}); e.click(); return true; })()"
            % (json.dumps(selector), json.dumps(bool(text)).lower(), json.dumps(text), nth))
        if not ok:
            raise LookupError(f"nothing to click: {selector!r} {text!r} #{nth}")

    def exists(self, selector: str, text: str = "") -> bool:
        return bool(self.eval(
            "[...document.querySelectorAll(%s)].some(e => !%s || (e.innerText || e.textContent || '').includes(%s))" % (json.dumps(selector), json.dumps(bool(text)).lower(), json.dumps(text))))

    # ── the SQL editor (Monaco) ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
    def set_editor(self, sql: str):
        self.eval("window.monaco.editor.getEditors()[0].setValue(%s)" % json.dumps(sql))

    def close(self):
        try:
            self.ws.close()
        except Exception:
            pass


class Processes:
    """The things a screen test starts (the Vite dev server, the app) and ends again; one that was running before is left alone."""

    def __init__(self):
        self.started: list[subprocess.Popen] = []

    def vite(self):
        if port_open(VITE_PORT):
            return
        p = subprocess.Popen("npm run dev", cwd=FRONTEND, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.started.append(p)
        wait_for(lambda: port_open(VITE_PORT), 90, what="the Vite dev server")

    def app(self, port: int = 9222) -> Page:
        if port_open(port):
            return Page(port)
        env = dict(os.environ, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=f"--remote-debugging-port={port}")
        p = subprocess.Popen([str(APP_EXE)], env=env, cwd=str(APP_EXE.parent), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.started.append(p)
        wait_for(lambda: port_open(port), 60, what="the app's DevTools port")
        time.sleep(2)
        return Page(port)

    def stop(self):
        for p in reversed(self.started):
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(p.pid)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.started.clear()


class TempConnection:
    """A saved connection of its own for the test (a data folder of its own, root / root, signs in at once), put into code/data/connections.json and taken out again: the file the
    user keeps is restored byte for byte, whatever happens."""

    def __init__(self, name: str, data_dir: Path):
        self.name = name
        self.data_dir = data_dir
        self.backup = CONNECTIONS.read_bytes() if CONNECTIONS.exists() else None

    def __enter__(self):
        self.data_dir.mkdir(parents=True, exist_ok=True)
        connections = json.loads(self.backup.decode("utf-8")) if self.backup else []
        connections.append({"id": "cdp-test", "name": self.name, "host": "localhost", "port": 7878, "user": "root", "password": "root", "autoLogin": True,
                            "dataDir": str(self.data_dir), "favorite": False, "lastUsed": int(time.time() * 1000)})
        CONNECTIONS.write_text(json.dumps(connections, ensure_ascii=False, indent=2), encoding="utf-8")
        return self

    def __exit__(self, *exc):
        if self.backup is None:
            CONNECTIONS.unlink(missing_ok=True)
        else:
            CONNECTIONS.write_bytes(self.backup)
        shutil.rmtree(self.data_dir, ignore_errors=True)
