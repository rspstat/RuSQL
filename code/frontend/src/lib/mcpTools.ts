// mcpTools.ts
// The tools of the MCP server (code/mcp/mcp_server.py) as the AI MCP panel of the Server Manager lists them: grouped, each with a request that would make Claude use it. The names
// are the server's own (code/test/frontend/test_libs.mjs compares this list with the `@mcp.tool()` functions of mcp_server.py, so that the panel cannot drift from the server
// again: it once listed tools that had been renamed or never existed). No DOM here.

export interface McpTool {
  name: string;
  example: string; // what the user could ask Claude
}

export interface McpToolGroup {
  title: string;
  accent: "teal" | "violet" | "amber"; // the colour of the group's chips
  tools: McpTool[];
}

export const MCP_TOOL_GROUPS: McpToolGroup[] = [
  {
    title: "Database tools",
    accent: "teal",
    tools: [
      { name: "execute_sql", example: "Show the ten newest orders" },
      { name: "confirm_dangerous_sql", example: "Yes, drop that table (after Claude asked)" },
      { name: "run_script", example: "Preview these three updates, then apply them" },
      { name: "list_databases", example: "Which databases are there?" },
      { name: "list_tables", example: "List the tables of shop" },
      { name: "get_table_schema", example: "Show the CREATE TABLE of orders" },
      { name: "explain_query", example: "Why is this SELECT slow?" },
      { name: "get_indexes", example: "Which indexes does orders have?" },
      { name: "suggest_indexes", example: "Which index would speed this query up? Try it" },
      { name: "slow_queries", example: "Which queries have been slow lately?" },
      { name: "list_locks", example: "Is something blocking my update?" },
      { name: "kill_session", example: "Stop session 12 (Claude asks first)" },
      { name: "health_report", example: "How is the server doing?" },
      { name: "get_table_stats", example: "How many different values does status have?" },
      { name: "get_fk_graph", example: "How do customers and invoices relate?" },
      { name: "trace_query", example: "Where does this statement spend its time?" },
      { name: "sample_data", example: "Show me a few rows of customers" },
      { name: "import_csv", example: "Load C:\\data\\customers.csv into customers" },
      { name: "export_csv", example: "Save this result as a CSV file" },
      { name: "diff_schema", example: "How does staging differ from production?" },
    ],
  },
  {
    title: "Connection and editor tools",
    accent: "violet",
    tools: [
      { name: "list_connections", example: "Which connections are saved?" },
      { name: "add_connection", example: "Add a connection to 127.0.0.1:7878" },
      { name: "update_connection", example: "Change the port of local-dev" },
      { name: "delete_connection", example: "Remove the connection staging" },
      { name: "write_to_editor", example: "Write a query to select all from users" },
      { name: "new_editor_tab", example: "Write a monthly aggregate query in a new tab" },
      { name: "execute_in_editor", example: "Run it in the editor" },
      { name: "close_editor_tab", example: "Close the query1.sql tab" },
      { name: "get_editor_tab_content", example: "Read the content of the report.sql tab" },
      { name: "list_editor_tabs", example: "Show me the list of open tabs" },
      { name: "switch_editor_tab", example: "Switch to the report.sql tab" },
    ],
  },
  {
    title: "App and server control",
    accent: "amber",
    tools: [
      { name: "list_app_instances", example: "Which RuSQL windows are open?" },
      { name: "launch_app", example: "Start RuSQL" },
      { name: "login", example: "Log in with local-dev" },
      { name: "start_server", example: "Start the server on port 7878" },
      { name: "stop_server", example: "Stop the server" },
      { name: "get_server_status", example: "Is the server running?" },
    ],
  },
];

export const MCP_TOOL_NAMES: string[] = MCP_TOOL_GROUPS.flatMap(g => g.tools.map(t => t.name));
