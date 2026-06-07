"""
RustDB True MCP Server
stdio transport — Claude Desktop spawns this process and communicates via JSON-RPC.

Claude Desktop config:
  %APPDATA%\\Claude\\claude_desktop_config.json
  → see claude_desktop_config_example.json
"""
import socket
import sys
from mcp.server.fastmcp import FastMCP

RUSTDB_HOST = "127.0.0.1"
RUSTDB_PORT = 7878
RUSTDB_USER = "root"
RUSTDB_PASS = "root"

mcp = FastMCP("RustDB v2.2.0")


class _Conn:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((RUSTDB_HOST, RUSTDB_PORT))
        self.sock.settimeout(30)
        self._recv()  # welcome banner
        self._raw(f"AUTH {RUSTDB_USER} {RUSTDB_PASS}")

    def _raw(self, msg: str) -> str:
        self.sock.sendall((msg + "\n").encode())
        return self._recv()

    def _recv(self) -> str:
        buf = ""
        while True:
            buf += self.sock.recv(4096).decode(errors="replace")
            if "---END---" in buf:
                return buf.replace("---END---", "").strip()

    def exec(self, sql: str, db: str = "") -> str:
        if db:
            self._raw(f"USE {db};")
        q = sql.strip()
        if not q.endswith(";"):
            q += ";"
        return self._raw(q)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def _run(sql: str, db: str = "") -> str:
    print(f"[mcp] _run called: db={db!r} sql={sql[:60]!r}", file=sys.stderr, flush=True)
    try:
        c = _Conn()
        try:
            result = c.exec(sql, db) or "(empty result)"
            print(f"[mcp] result: {result[:80]!r}", file=sys.stderr, flush=True)
            return result
        finally:
            c.close()
    except ConnectionRefusedError:
        msg = f"Error: RustDB is not running on {RUSTDB_HOST}:{RUSTDB_PORT}. Start the server first."
        print(f"[mcp] {msg}", file=sys.stderr, flush=True)
        return msg
    except Exception as e:
        msg = f"Error: {e}"
        print(f"[mcp] {msg}", file=sys.stderr, flush=True)
        return msg


@mcp.tool()
def execute_sql(sql: str, database: str = "") -> str:
    """Execute any SQL query on RustDB and return the result.
    Optionally specify a database to USE before executing."""
    return _run(sql, database)


@mcp.tool()
def list_databases() -> str:
    """List all databases available in RustDB."""
    return _run("SHOW DATABASES")


@mcp.tool()
def list_tables(database: str = "") -> str:
    """List all tables in the specified database (default: current database)."""
    return _run("SHOW TABLES", database)


@mcp.tool()
def get_table_schema(table: str, database: str = "") -> str:
    """Get the CREATE TABLE DDL for a specific table."""
    return _run(f"SHOW CREATE TABLE {table}", database)


if __name__ == "__main__":
    mcp.run()
