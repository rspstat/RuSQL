"""Does what an installed RuSQL carries work on this machine? (macOS CI: the .app that `tauri build` made; any OS: a folder that holds engine/ python/ mcp/.)

  python3 smoke_bundle.py <folder with engine/ python/ mcp/>        (macOS: RuSQL.app/Contents/Resources)

It starts the bundled engine, talks to it over the native protocol and the MySQL protocol, and asks the MCP server for its tools with the bundled Python and the
`mcp` package in it. No window is opened: this is the part of the app that does not need a screen.
"""
import asyncio
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

if len(sys.argv) < 2:
    sys.exit(__doc__)
ROOT = Path(sys.argv[1]).resolve()
IS_WIN = os.name == "nt"
ENGINE = ROOT / "engine" / ("engine_server.exe" if IS_WIN else "engine_server")
PYTHON = ROOT / "python" / ("python.exe" if IS_WIN else "bin/python3")
MCP_SERVER = ROOT / "mcp" / "mcp_server.py"
PORT, MYSQL_PORT = 17878, 13306

failures = []


def check(name, ok, detail=""):
    print(("PASS  " if ok else "FAIL  ") + name + (f"  -- {detail}" if detail and not ok else ""))
    if not ok:
        failures.append(name)


def wait_port(port, seconds=20):
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def main():
    for what, path in (("engine", ENGINE), ("python", PYTHON), ("mcp_server.py", MCP_SERVER)):
        check(f"{what} is in the bundle ({path})", path.exists() and path.stat().st_size > 10_000)
    if failures:
        return

    data = tempfile.mkdtemp(prefix="rusql-smoke-")
    engine = subprocess.Popen([str(ENGINE), "--port", str(PORT), "--mysql-port", str(MYSQL_PORT), "--data-dir", data, "--buffer-pool-size", "64"], cwd=data,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        check("the engine accepts connections (native protocol)", wait_port(PORT))
        check("the engine accepts connections (MySQL protocol)", wait_port(MYSQL_PORT))

        # the MySQL wire protocol: the server speaks first, a handshake packet whose first payload byte is the protocol version 10
        with socket.create_connection(("127.0.0.1", MYSQL_PORT), timeout=5) as s:
            head = s.recv(64)
        check("MySQL handshake (protocol version 10, server version text)", len(head) > 10 and head[4] == 10 and b"8.4" in head[:40], repr(head[:40]))

        # the MCP server, started the way Claude Desktop starts it (the bundled Python, -B so that no .pyc lands in the bundle)
        from_here = subprocess.run([str(PYTHON), "-B", "-c", "from mcp.server.fastmcp import FastMCP; print('mcp ok')"], capture_output=True, text=True, cwd=data)
        check("the bundled Python imports the mcp package", from_here.returncode == 0 and "mcp ok" in from_here.stdout, from_here.stderr[-300:])

        async def talk():
            from mcp import ClientSession, StdioServerParameters
            from mcp.client.stdio import stdio_client

            params = StdioServerParameters(command=str(PYTHON), args=["-B", "-u", str(MCP_SERVER)],
                                           env={**os.environ, "RUSQL_HOST": "127.0.0.1", "RUSQL_PORT": str(PORT), "RUSQL_USER": "root", "RUSQL_PASS": "root",
                                                "RUSQL_DATA_DIR": data, "PYTHONDONTWRITEBYTECODE": "1"})
            async with stdio_client(params) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    tools = (await session.list_tools()).tools
                    check("the MCP server lists its 37 tools", len(tools) == 37, str(len(tools)))
                    check("16 of them are marked read-only", sum(1 for t in tools if t.annotations and t.annotations.readOnlyHint) == 16)
                    made = await session.call_tool("execute_sql", {"sql": "CREATE DATABASE smoke"})
                    check("execute_sql creates a database", not made.isError, str(made.content)[:200])
                    await session.call_tool("execute_sql", {"sql": "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20))", "database": "smoke"})
                    await session.call_tool("execute_sql", {"sql": "INSERT INTO t VALUES (1, 'mac'), (2, 'ok')", "database": "smoke"})
                    rows = await session.call_tool("execute_sql", {"sql": "SELECT id, name FROM t ORDER BY id", "database": "smoke"})
                    text = "".join(c.text for c in rows.content if hasattr(c, "text"))
                    check("execute_sql reads the rows back", '"mac"' in text and '"ok"' in text, text[:200])
                    dbs = await session.call_tool("list_databases", {})
                    check("list_databases shows the new database", "smoke" in "".join(c.text for c in dbs.content if hasattr(c, "text")))
                    health = await session.call_tool("health_report", {})
                    check("health_report answers", not health.isError and "uptime" in "".join(c.text for c in health.content if hasattr(c, "text")).lower())

        asyncio.run(talk())

        # the data folder is the engine's: files were written (the engine really stored what the tool created)
        stored = [p.name for p in Path(data).rglob("*") if p.is_file()]
        check("the engine wrote its files into the data folder", any(n.endswith(".rdb") or n.endswith(".schema.json") for n in stored), str(stored[:6]))
        pyc = [p for p in ROOT.rglob("*.pyc") if "python" in p.parts and "site-packages" in p.parts and p.stat().st_mtime > time.time() - 600]
        check("running the bundled Python wrote no .pyc into the bundle", not pyc, str(pyc[:3]))
    finally:
        engine.terminate()
        try:
            engine.wait(timeout=10)
        except subprocess.TimeoutExpired:
            engine.kill()
        check("the engine stops on SIGTERM / terminate", engine.returncode is not None)


main()
print("\n%s" % ("ALL PASSED" if not failures else f"{len(failures)} FAILED: " + ", ".join(failures)))
sys.exit(1 if failures else 0)
