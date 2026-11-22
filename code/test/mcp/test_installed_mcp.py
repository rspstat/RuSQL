"""The MCP part of an INSTALLED RuSQL, the way Claude Desktop uses it:  py -3 code/test/mcp/test_installed_mcp.py <folder with rusql-ui.exe, engine/ and mcp/>

 1. the tool names of mcp/mcp_server.py against the `alwaysAllow` list the app writes (setup_mcp_config in main.rs): every name is a real tool, and no destructive tool is on it,
 2. the installed mcp_server.py is started over stdio (the Windows Python with the `mcp` package) against an engine of the same folder started here (own ports, temp data folder),
 3. initialize, list the tools, call a handful of them for real (what one tool writes another reads), and a DROP DATABASE sent through execute_sql must be refused.
Only the engine this script started is stopped (its own Popen).
"""
import asyncio
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8")
install = Path(sys.argv[1]).resolve()
mcp_script = install / "mcp" / "mcp_server.py"
engine = install / "engine" / "engine_server.exe"
CODE = Path(__file__).resolve().parents[2]
failures = []


def check(ok, what, detail=""):
    print(("PASS  " if ok else "FAIL  ") + what + (("   [%s]" % detail) if detail and not ok else ""))
    if not ok:
        failures.append(what)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# 1. the names
src = mcp_script.read_text(encoding="utf-8")
tools = re.findall(r"@mcp\.tool\([^)]*\)\s*\n(?:async\s+)?def\s+(\w+)", src)
check(len(tools) == len(set(tools)) == 37, "mcp_server.py defines 37 distinct tools", "%d / %d" % (len(tools), len(set(tools))))
rust = (CODE / "frontend" / "src-tauri" / "src" / "main.rs").read_text(encoding="utf-8")
allowed = re.findall(r'"(\w+)"', re.search(r'"alwaysAllow":\s*\[(.*?)\]', rust, re.S).group(1))
check(set(allowed) <= set(tools), "every tool in alwaysAllow really exists", str(sorted(set(allowed) - set(tools))))
destructive = {"delete_connection", "update_connection", "confirm_dangerous_sql", "kill_session", "import_csv", "stop_server"} & set(allowed)
check(not destructive, "no destructive tool is on the always-allowed list", str(sorted(destructive)))

# 2. an engine of the installed folder + the installed MCP server over stdio
port, mysql_port = free_port(), free_port()
data = Path(tempfile.mkdtemp(prefix="rusql_mcp_check_"))
(data / "engine_data").mkdir()
eng = subprocess.Popen([str(engine), "--port", str(port), "--data-dir", str(data / "engine_data"), "--mysql-port", str(mysql_port)], cwd=str(data / "engine_data"),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=0x08000000)
try:
    for _ in range(60):
        try:
            socket.create_connection(("127.0.0.1", port), 1).close()
            break
        except OSError:
            time.sleep(0.25)
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    env = dict(os.environ, RUSQL_HOST="127.0.0.1", RUSQL_PORT=str(port), RUSQL_USER="root", RUSQL_PASS="root", RUSQL_DATA_DIR=str(data))

    async def go():
        params = StdioServerParameters(command=sys.executable, args=["-u", str(mcp_script)], env=env)
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as session:
                await session.initialize()
                listed = await session.list_tools()
                check(sorted(t.name for t in listed.tools) == sorted(tools), "the running server lists exactly the tools of the file", "%d names" % len(listed.tools))
                check(all((t.description or "").strip() for t in listed.tools), "every tool has a description (what Claude reads to choose it)",
                      str([t.name for t in listed.tools if not (t.description or "").strip()]))

                async def call(name, **args):
                    r = await session.call_tool(name, args)
                    return "".join(getattr(c, "text", "") for c in r.content)

                out = {}
                await call("execute_sql", sql="CREATE DATABASE mcpcheck")
                await call("execute_sql", sql="CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(20))", database="mcpcheck")
                await call("execute_sql", sql="INSERT INTO t VALUES (1,'Kim'),(2,'Lee'),(3,'Park')", database="mcpcheck")
                out["dbs"] = await call("list_databases")
                out["tables"] = await call("list_tables", database="mcpcheck")
                out["schema"] = await call("get_table_schema", database="mcpcheck", table="t")
                out["sample"] = await call("sample_data", database="mcpcheck", table="t")
                out["select"] = await call("execute_sql", sql="SELECT COUNT(*) AS n FROM t", database="mcpcheck")
                out["explain"] = await call("explain_query", sql="SELECT * FROM t WHERE id = 2", database="mcpcheck")
                out["health"] = await call("health_report")
                out["locks"] = await call("list_locks")
                out["stats"] = await call("get_table_stats", database="mcpcheck", table="t")
                out["indexes"] = await call("get_indexes", database="mcpcheck", table="t")
                out["danger"] = await call("execute_sql", sql="DROP DATABASE mcpcheck", database="mcpcheck")
                out["still"] = await call("list_tables", database="mcpcheck")
                out["instances"] = await call("list_app_instances")
                return out

    out = asyncio.run(go())
    check("mcpcheck" in out["dbs"], "list_databases sees the database made by execute_sql", out["dbs"][:120])
    check("t" in out["tables"], "list_tables sees the table", out["tables"][:120])
    check("id" in out["schema"] and "name" in out["schema"], "get_table_schema lists the columns", out["schema"][:160])
    check("Kim" in out["sample"] and "Park" in out["sample"], "sample_data returns the rows", out["sample"][:160])
    check("3" in out["select"], "execute_sql reads back the 3 rows", out["select"][:160])
    check(out["explain"].strip() != "", "explain_query answers", out["explain"][:120])
    check(out["health"].strip() != "" and "rror" not in out["health"][:200], "health_report answers", out["health"][:160])
    check(out["locks"].strip() != "", "list_locks answers", out["locks"][:120])
    check(out["stats"].strip() != "" and "rror" not in out["stats"][:120], "get_table_stats answers", out["stats"][:160])
    check(out["indexes"].strip() != "", "get_indexes answers", out["indexes"][:120])
    check(out["danger"].startswith("Refused") and "t" in out["still"], "a DROP DATABASE sent through execute_sql is refused (the table is still there)", out["danger"][:200] + " | " + out["still"][:80])
    check(out["instances"].strip() != "", "list_app_instances answers (the registry of the app windows)", out["instances"][:160])
finally:
    eng.kill()
    eng.wait()
print("\nALL PASS" if not failures else "\n%d FAILED: %s" % (len(failures), "; ".join(failures)))
sys.exit(1 if failures else 0)
