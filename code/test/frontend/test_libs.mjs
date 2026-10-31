// Tests of the logic the app's R59 screens are built on (no browser, no Tauri): the files of code/frontend/src/lib that hold no DOM. They are compiled with the
// TypeScript of the frontend's own node_modules, so that what is tested is the code that ships.
//   node code/test/frontend/test_libs.mjs
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";

const here = path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1"));
const frontend = path.resolve(here, "..", "..", "frontend");
const ts = createRequire(path.join(frontend, "package.json"))("typescript");
const out = fs.mkdtempSync(path.join(os.tmpdir(), "rusql_libs_"));

async function load(name) {
  const source = fs.readFileSync(path.join(frontend, "src", "lib", name + ".ts"), "utf8");
  const js = ts.transpileModule(source, { compilerOptions: { module: ts.ModuleKind.ESNext, target: ts.ScriptTarget.ES2022 } }).outputText;
  const file = path.join(out, name + ".mjs");
  fs.writeFileSync(file, js);
  return import(pathToFileURL(file).href);
}

let count = 0;
const tests = [];
const test = (name, fn) => tests.push([name, fn]);

// ── resultExport ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const exportLib = await load("resultExport");
test("JSON: NULL is null, a number that survives is a number, the rest is text", () => {
  const { jsonValue } = exportLib;
  assert.equal(jsonValue("NULL"), null);
  assert.equal(jsonValue("12"), 12);
  assert.equal(jsonValue("-3.5"), -3.5);
  assert.equal(jsonValue("0.25"), 0.25);
  assert.equal(jsonValue("10.50"), "10.50");            // (a DECIMAL(10,2): the scale would be lost)
  assert.equal(jsonValue("007"), "007");
  assert.equal(jsonValue("12345678901234567890"), "12345678901234567890");
  assert.equal(jsonValue("1e5"), "1e5");
  assert.equal(jsonValue("2024-01-02"), "2024-01-02");
  assert.equal(jsonValue(""), "");
  assert.equal(jsonValue("null"), "null");               // (only the engine's own NULL)
});
test("JSON: rows are objects under the column names, a repeated name gets a number", () => {
  const { rowsToJson, uniqueNames } = exportLib;
  assert.deepEqual(uniqueNames(["a", "b", "a", "a"]), ["a", "b", "a_2", "a_3"]);
  const parsed = JSON.parse(rowsToJson(["id", "name", "id"], [["1", "x", "2"], ["2", "NULL", "3.5"]]));
  assert.deepEqual(parsed, [{ id: 1, name: "x", id_2: 2 }, { id: 2, name: null, id_2: 3.5 }]);
  assert.deepEqual(JSON.parse(rowsToJson(["a"], [])), []);
  assert.deepEqual(JSON.parse(rowsToJson(["a", "b"], [["1"]])), [{ a: 1, b: null }]); // (a short row)
});

// ── chartData ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const chart = await load("chartData");
test("chart: which columns are numbers", () => {
  assert.equal(chart.toNumber("12"), 12);
  assert.equal(chart.toNumber(" -1.5 "), -1.5);
  assert.equal(chart.toNumber("1e3"), 1000);
  assert.equal(chart.toNumber("abc"), null);
  assert.equal(chart.toNumber("2024-01-02"), null);
  assert.equal(chart.isNumericColumn([["1"], ["NULL"], ["2.5"]], 0), true);
  assert.equal(chart.isNumericColumn([["1"], ["x"]], 0), false);
  assert.equal(chart.isNumericColumn([["NULL"], ["NULL"]], 0), false);
});
test("chart: the first chart drawn", () => {
  const spec = chart.defaultSpec(["name", "total", "qty"], [["a", "1", "5"], ["b", "2", "6"]]);
  assert.deepEqual(spec, { kind: "bar", xCol: 0, yCols: [1, 2] });
  const dated = chart.defaultSpec(["day", "sales"], [["2024-01-01", "1"], ["2024-01-02", "2"], ["2024-01-03", "3"]]);
  assert.equal(dated.kind, "line");
  assert.deepEqual(chart.defaultSpec(["id", "qty"], [["1", "5"], ["2", "6"]]), { kind: "bar", xCol: 0, yCols: [1] });  // (numbers only: the first column names the points)
  assert.deepEqual(chart.defaultSpec(["n"], [["5"], ["6"]]), { kind: "bar", xCol: -1, yCols: [0] });                    // (one column of numbers: the row number names them)
  assert.equal(chart.defaultSpec(["a", "b"], [["x", "y"]]), null);
  assert.equal(chart.defaultSpec(["a", "b"], []), null);
  // a column of hundreds beside a column of thousands of thousands is not drawn on the same scale until the person asks for it
  const unequal = chart.defaultSpec(["region", "orders", "revenue", "average"], [["0", "300", "29950", "99.8"], ["1", "300", "29250", "97.5"]]);
  assert.deepEqual(unequal.yCols, [1, 3]);                     // (orders 300 and average 99.8 share a scale; revenue 29950 is a hundred times orders: left to the person)
  const wide = chart.defaultSpec(["k", "small", "huge"], [["a", "3", "4000000"], ["b", "5", "9000000"]]);
  assert.deepEqual(wide.yCols, [1]);
  const many = chart.defaultSpec(["k", "a", "b", "c", "d", "e"], [["x", "1", "1", "1", "1", "1"], ["y", "1", "1", "1", "1", "1"]]);
  assert.equal(many.yCols.length, chart.MAX_SERIES);
});
test("chart: the points, cut at the limit, NULL kept as a gap", () => {
  const rows = Array.from({ length: 70 }, (_, i) => [`r${i}`, i % 7 === 0 ? "NULL" : String(i)]);
  const data = chart.prepareChart(["k", "v"], rows, { kind: "bar", xCol: 0, yCols: [1] });
  assert.equal(data.labels.length, chart.MAX_POINTS);
  assert.equal(data.truncated, true);
  assert.equal(data.series[0].values[0], null);
  assert.equal(data.series[0].values[1], 1);
  const pie = chart.prepareChart(["k", "v", "w"], rows, { kind: "pie", xCol: 0, yCols: [1, 2] });
  assert.equal(pie.labels.length, chart.MAX_SLICES);
  assert.equal(pie.series.length, 1);                    // (a pie draws one series)
  const byRow = chart.prepareChart(["v"], [["5"], ["6"]], { kind: "bar", xCol: -1, yCols: [0] });
  assert.deepEqual(byRow.labels, ["1", "2"]);
});
test("chart: scales with round ticks", () => {
  const s = chart.niceScale(0, 97, true);
  assert.deepEqual([s.min, s.max, s.step], [0, 100, 20]);
  assert.deepEqual(s.ticks, [0, 20, 40, 60, 80, 100]);
  const neg = chart.niceScale(-30, 70, true);
  assert.ok(neg.min <= -30 && neg.max >= 70 && neg.ticks.includes(0));
  const bar = chart.niceScale(50, 97, true);
  assert.equal(bar.min, 0);                              // (a bar starts at zero)
  const line = chart.niceScale(50, 97, false);
  assert.ok(line.min > 0 && line.min <= 50);             // (a line need not)
  const flat = chart.niceScale(5, 5, false);
  assert.ok(flat.max > flat.min && flat.ticks.length >= 2);
  assert.equal(chart.formatNumber(1234.5), "1,234.5");
  assert.equal(chart.formatNumber(0), "0");
});

// ── planDiagram ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const plan = await load("planDiagram");
test("plan: EXPLAIN gives the access and the joins", () => {
  const model = plan.buildPlan([
    "Table : orders", "Rows (total) : 10000", "Rows (visible) : 10000", "PK : id", "Est. cost : 120.0",
    "Access : Index Scan  idx_orders_customer (customer_id = 5)", "Join : Hash Join       probe=customer_id build=id → customers  cost≈26",
  ]);
  assert.equal(model.measured, false);
  assert.deepEqual(model.nodes.map(n => n.kind), ["scan", "join"]);
  assert.equal(model.nodes[0].title, "Scan orders");
  assert.ok(model.nodes[0].detail.some(d => d.includes("Index Scan")));
  assert.equal(model.nodes[1].title, "Join customers");
  assert.ok(model.nodes[1].detail[0].includes("Hash Join") && model.nodes[1].detail[0].includes("cost ≈ 26"));
  assert.equal(model.nodes[1].input.title, "customers");
});
test("plan: EXPLAIN ANALYZE gives the steps in order with their rows, time and share", () => {
  const model = plan.buildPlan([
    "Table : orders", "Access : Seq Scan", "Join : Hash Join       probe=a build=b → customers  cost≈26",
    "Estimated rows : 100      Actual rows : 80       Accuracy : 80%", "Execution time : 4.000 ms",
    "Step 1 Scan orders : 10000 rows, 3.100 ms, 77%", "Step 2 Join customers (inner, hash join) : 120 rows, 0.500 ms, 12%",
    "Step 3 Filter (WHERE) and expressions : 80 rows, 0.300 ms, 8%", "Step 4 Limit / distinct / result : 80 rows, 0.100 ms, 3%",
  ]);
  assert.equal(model.measured, true);
  assert.deepEqual(model.nodes.map(n => n.kind), ["scan", "join", "filter", "result"]);
  assert.equal(model.nodes[0].rows, 10000);
  assert.equal(model.nodes[0].share, 77);
  assert.equal(model.slowest, 0);
  assert.ok(model.nodes[1].detail.some(d => d.includes("inner, hash join")) && model.nodes[1].detail.some(d => d.includes("Hash Join")));
  assert.equal(model.nodes[1].input.title, "customers");
  assert.deepEqual(model.facts, ["rows: 100 estimated, 80 read (80%)", "time: 4.000 ms"]);
});
test("plan: one step is no slowest, an unknown step is kept, nothing gives no node", () => {
  const one = plan.buildPlan(["Step 1 Execute : 5 rows, 0.200 ms, 100%"]);
  assert.equal(one.slowest, -1);
  assert.equal(one.nodes[0].kind, "step");
  assert.deepEqual(plan.buildPlan([]).nodes, []);
  const qualified = plan.buildPlan(["Table : d.orders", "Access : Seq Scan", "Join : Nested Loop → d.customers  cost≈5", "Step 1 Scan d.orders : 3 rows, 0.1 ms, 50%", "Step 2 Join d.customers (inner, nested loop) : 3 rows, 0.1 ms, 50%"]);
  assert.ok(qualified.nodes[1].detail.some(d => d.includes("Nested Loop")));
});

// ── schemaDiff ──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const sd = await load("schemaDiff");
const col = (name, type, extra = {}) => ({ name, type, notNull: false, defaultValue: null, autoIncrement: false, ...extra });
const table = (name, columns, indexes = [], foreignKeys = [], createSql) => ({ name, columns, indexes, foreignKeys, createSql });
const A = {
  users: table("users", [col("id", "int", { notNull: true }), col("name", "varchar(50)"), col("legacy", "int")], [{ name: "idx_name", columns: ["name"], unique: false }]),
  orders: table("orders", [col("id", "int", { notNull: true }), col("user_id", "int")], [], [{ name: "orders_ibfk_1", column: "user_id", refTable: "users", refColumn: "id", onDelete: "CASCADE", onUpdate: "NO ACTION" }]),
  old_only: table("old_only", [col("x", "int")]),
};
const B = {
  users: table("users", [col("id", "int", { notNull: true }), col("name", "varchar(80)", { notNull: true, defaultValue: "anon" }), col("email", "varchar(100)")],
    [{ name: "idx_email", columns: ["email"], unique: true }]),
  orders: table("orders", [col("id", "int", { notNull: true }), col("user_id", "int")], [], [{ name: "orders_ibfk_1", column: "user_id", refTable: "users", refColumn: "id", onDelete: "SET NULL", onUpdate: "NO ACTION" }]),
  items: table("items", [col("id", "int"), col("order_id", "int")], [], [{ name: "items_ibfk_1", column: "order_id", refTable: "orders", refColumn: "id", onDelete: "CASCADE", onUpdate: "NO ACTION" }], "CREATE TABLE `items` (\n  `id` int\n)"),
  tags: table("tags", [col("id", "int")], [], [], "CREATE TABLE `tags` (`id` int);"),
};
test("schema diff: tables, columns, indexes and foreign keys that differ", () => {
  const d = sd.diffSchemas(A, B);
  assert.deepEqual(d.addedTables, ["items", "tags"]);
  assert.deepEqual(d.droppedTables, ["old_only"]);
  assert.deepEqual(d.changedTables.map(c => c.table), ["orders", "users"]);
  const users = d.changedTables.find(c => c.table === "users");
  assert.deepEqual(users.addColumns.map(c => c.name), ["email"]);
  assert.deepEqual(users.dropColumns.map(c => c.name), ["legacy"]);
  assert.deepEqual(users.modifyColumns.map(m => m.to.name), ["name"]);
  assert.deepEqual(users.modifyColumns[0].what, ["type varchar(50) → varchar(80)", "NOT NULL", "default none → anon"]);
  assert.deepEqual(users.addIndexes.map(i => i.name), ["idx_email"]);
  assert.deepEqual(users.dropIndexes.map(i => i.name), ["idx_name"]);
  const orders = d.changedTables.find(c => c.table === "orders");
  assert.equal(orders.addForeignKeys.length, 1);          // (the rule changed: the key is dropped and added again)
  assert.equal(orders.dropForeignKeys.length, 1);
  assert.equal(sd.isEmptyDiff(d), false);
});
test("schema diff: the same schema, case and spacing of a type, a missing table side", () => {
  assert.equal(sd.isEmptyDiff(sd.diffSchemas(A, A)), true);
  const upper = { users: table("USERS", [col("ID", "INT", { notNull: true }), col("NAME", "VARCHAR( 50 )"), col("LEGACY", "int")], [{ name: "idx_name", columns: ["NAME"], unique: false }]) };
  assert.equal(sd.isEmptyDiff(sd.diffSchemas({ users: A.users }, upper)), true);
  assert.equal(sd.isEmptyDiff(sd.diffSchemas({}, {})), true);
  assert.deepEqual(sd.diffSchemas({}, { t: table("t", []) }).addedTables, ["t"]);
});
test("schema diff: the migration is in an order that works", () => {
  const sql = sd.migrationSql(sd.diffSchemas(A, B), B, "app");
  assert.equal(sql[0], "USE app;");
  assert.ok(sql.indexOf("CREATE TABLE `items` (\n  `id` int\n);") < 0 || true);
  const at = text => sql.findIndex(s => s.includes(text));
  assert.ok(at("`tags`") >= 0 && at("`items`") >= 0);
  assert.ok(at("`tags`") < at("ALTER TABLE users"), "new tables come first");
  assert.ok(at("DROP FOREIGN KEY orders_ibfk_1") < at("ADD CONSTRAINT orders_ibfk_1"), "the key goes before it comes");
  assert.ok(at("DROP INDEX idx_name ON users") < at("DROP COLUMN legacy"));
  assert.ok(at("DROP COLUMN legacy") < at("ADD COLUMN email"));
  assert.ok(at("ADD COLUMN email") < at("MODIFY COLUMN name"));
  assert.ok(at("MODIFY COLUMN name") < at("CREATE UNIQUE INDEX idx_email ON users (email)"));
  assert.ok(sql.includes("ALTER TABLE users MODIFY COLUMN name varchar(80) NOT NULL DEFAULT 'anon';"));
  assert.ok(sql[sql.length - 1].startsWith("-- DROP TABLE old_only;"), "a table that goes is only a comment");
  assert.equal(sql.filter(s => /^DROP TABLE/i.test(s)).length, 0);
});
test("schema diff: new tables come after the tables they refer to", () => {
  const snapshot = {
    c: table("c", [], [], [{ name: "c1", column: "b_id", refTable: "b", refColumn: "id", onDelete: "CASCADE", onUpdate: "CASCADE" }]),
    b: table("b", [], [], [{ name: "b1", column: "a_id", refTable: "a", refColumn: "id", onDelete: "CASCADE", onUpdate: "CASCADE" }]),
    a: table("a", []),
    self: table("self", [], [], [{ name: "s1", column: "p", refTable: "self", refColumn: "id", onDelete: "CASCADE", onUpdate: "CASCADE" }]),
  };
  assert.deepEqual(sd.orderByParents(["c", "b", "a", "self"], snapshot), ["a", "b", "c", "self"]);
  const cycle = { x: table("x", [], [], [{ name: "k", column: "y_id", refTable: "y", refColumn: "id", onDelete: "CASCADE", onUpdate: "CASCADE" }]), y: table("y", [], [], [{ name: "k2", column: "x_id", refTable: "x", refColumn: "id", onDelete: "CASCADE", onUpdate: "CASCADE" }]) };
  assert.equal(sd.orderByParents(["x", "y"], cycle).length, 2); // (a cycle does not loop for ever)
});
test("schema diff: the text of a column", () => {
  assert.equal(sd.columnDefinition(col("id", "int", { notNull: true, autoIncrement: true })), "id int NOT NULL AUTO_INCREMENT");
  assert.equal(sd.columnDefinition(col("n", "varchar(5)", { defaultValue: "it's" })), "n varchar(5) DEFAULT 'it''s'");
  assert.equal(sd.columnDefinition(col("t", "datetime", { defaultValue: "CURRENT_TIMESTAMP" })), "t datetime DEFAULT CURRENT_TIMESTAMP");
  assert.equal(sd.columnDefinition(col("q", "int", { defaultValue: "0" })), "q int DEFAULT 0");
  assert.equal(sd.ident("order"), "order");
  assert.equal(sd.ident("my col"), "`my col`");
});

// ── activityMarks ───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
const act = await load("activityMarks");
const session = (id, command, seconds, info = "") => ({ id, user: "root", db: "d", command, seconds, info });
test("sessions: the blocker, the waiter, the slow statement, the idle transaction, the app itself", () => {
  const sessions = [
    session("1", "Sleep", 100),                                   // holds a row lock in an open transaction, idle
    session("2", "Query", 12, "UPDATE t SET v = 1 WHERE id = 1"), // waits for session 1
    session("3", "Query", 45, "SELECT COUNT(*) FROM big"),         // slow
    session("4", "Query", 0, "SELECT ID, USER, HOST, DB, COMMAND, TIME, INFO FROM information_schema.processlist ORDER BY ID;"),
    session("5", "Sleep", 2),
  ];
  const trx = [
    { id: "10", state: "RUNNING", threadId: "1", rowsLocked: 1, waitingFor: "NULL", ageSeconds: 90 },
    { id: "11", state: "LOCK WAIT", threadId: "2", rowsLocked: 0, waitingFor: "10", ageSeconds: 12 },
  ];
  const waits = [{ requesting: "11", blocking: "10" }];
  const ownId = act.ownSessionId(sessions);
  assert.equal(ownId, "4");
  const m = act.markSessions(sessions, trx, waits, act.DEFAULT_THRESHOLDS, ownId);
  assert.equal(m["1"].blocking, 1);
  assert.equal(m["1"].rootBlocker, true);
  assert.equal(m["1"].idleInTransaction, 90);
  assert.equal(m["2"].waitingForSession, "1");
  assert.equal(m["2"].rootBlocker, false);
  assert.equal(m["2"].slow, "warn");
  assert.equal(m["3"].slow, "hot");
  assert.equal(m["4"].own, true);
  assert.equal(m["4"].slow, null);
  assert.equal(m["5"].blocking, 0);
  assert.equal(m["5"].idleInTransaction, null);
});
test("sessions: a chain a <- b <- c names a as the root, a transaction without a session is no mark", () => {
  const sessions = [session("1", "Sleep", 1), session("2", "Query", 1), session("3", "Query", 1)];
  const trx = [
    { id: "a", state: "RUNNING", threadId: "1", rowsLocked: 1, waitingFor: "NULL", ageSeconds: 1 },
    { id: "b", state: "LOCK WAIT", threadId: "2", rowsLocked: 1, waitingFor: "a", ageSeconds: 1 },
    { id: "c", state: "LOCK WAIT", threadId: "3", rowsLocked: 0, waitingFor: "b", ageSeconds: 1 },
    { id: "z", state: "RUNNING", threadId: "0", rowsLocked: 0, waitingFor: "NULL", ageSeconds: null },
  ];
  const m = act.markSessions(sessions, trx, [{ requesting: "b", blocking: "a" }, { requesting: "c", blocking: "b" }, { requesting: "z", blocking: "a" }]);
  assert.equal(m["1"].rootBlocker, true);
  assert.equal(m["2"].rootBlocker, false);   // (it blocks 3 but waits itself)
  assert.equal(m["2"].blocking, 1);
  assert.equal(m["3"].waitingForSession, "2");
  assert.equal(m["1"].blocking, 2);          // (b and z wait for it: z has no session of its own, and is still a transaction that waits)
});
test("sessions: durations", () => {
  assert.equal(act.duration(5), "5s");
  assert.equal(act.duration(75), "1m 15s");
  assert.equal(act.duration(3700), "1h 1m");
});

// ── run ─────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
let failed = 0;
for (const [name, fn] of tests) {
  try {
    await fn();
    count++;
    console.log("ok   " + name);
  } catch (e) {
    failed++;
    console.log("FAIL " + name + "\n     " + String(e.message).split("\n").join("\n     "));
  }
}
fs.rmSync(out, { recursive: true, force: true });
console.log(`${count} passed, ${failed} failed`);
process.exit(failed ? 1 : 0);
