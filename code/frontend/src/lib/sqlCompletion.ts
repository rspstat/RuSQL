// sqlCompletion.ts
// Schema-aware completion for the SQL editors: after FROM / JOIN / INTO / UPDATE the tables of the current database, after `alias.` or `table.` the columns of
// that table, elsewhere the columns of the tables the statement names plus the tables, the functions and the keywords. The tables and columns are read from the
// engine (get_tables / get_columns_detail) the first time they are asked for and kept for a few seconds; the editor clears them after every statement it runs.

import { invoke } from "@tauri-apps/api/core";
import type * as Monaco from "monaco-editor";
import type { ColumnDetail } from "../types";

const TTL_MS = 8000;

const KEYWORDS = [
  "SELECT", "FROM", "WHERE", "GROUP BY", "ORDER BY", "HAVING", "LIMIT", "OFFSET", "DISTINCT", "AS", "JOIN", "LEFT JOIN", "RIGHT JOIN", "INNER JOIN", "CROSS JOIN",
  "ON", "USING", "AND", "OR", "NOT", "IN", "BETWEEN", "LIKE", "IS NULL", "IS NOT NULL", "EXISTS", "UNION", "UNION ALL", "INTERSECT", "EXCEPT", "WITH", "CASE", "WHEN",
  "THEN", "ELSE", "END", "ASC", "DESC", "INSERT INTO", "VALUES", "ON DUPLICATE KEY UPDATE", "UPDATE", "SET", "DELETE FROM", "REPLACE INTO", "CREATE TABLE",
  "CREATE INDEX", "CREATE VIEW", "ALTER TABLE", "DROP TABLE", "TRUNCATE TABLE", "PRIMARY KEY", "FOREIGN KEY", "REFERENCES", "UNIQUE", "DEFAULT", "AUTO_INCREMENT",
  "NOT NULL", "BEGIN", "COMMIT", "ROLLBACK", "SAVEPOINT", "EXPLAIN", "EXPLAIN ANALYZE", "SHOW TABLES", "SHOW LOCKS", "SHOW PROCESSLIST", "USE", "OVER",
  "PARTITION BY", "FOR UPDATE", "FILTER",
];

const FUNCTIONS = [
  "COUNT", "SUM", "AVG", "MIN", "MAX", "ANY_VALUE", "GROUP_CONCAT", "JSON_AGG", "STDDEV", "VARIANCE", "BIT_AND", "BIT_OR", "COALESCE", "IFNULL", "NULLIF", "IF",
  "CAST", "CONCAT", "CONCAT_WS", "SUBSTRING", "LENGTH", "CHAR_LENGTH", "UPPER", "LOWER", "TRIM", "REPLACE", "LEFT", "RIGHT", "LPAD", "RPAD", "INSTR", "ROUND",
  "FLOOR", "CEIL", "ABS", "MOD", "POW", "SQRT", "NOW", "CURDATE", "DATE", "DATE_ADD", "DATE_SUB", "DATEDIFF", "DATE_FORMAT", "YEAR", "MONTH", "DAY", "HOUR",
  "ROW_NUMBER", "RANK", "DENSE_RANK", "LAG", "LEAD", "NTILE", "FIRST_VALUE", "LAST_VALUE", "JSON_EXTRACT", "JSON_OBJECT", "JSON_ARRAY", "LAST_INSERT_ID",
];

// words after which the next name is a table
const TABLE_AFTER = /(?:\bFROM|\bJOIN|\bINTO|\bUPDATE|\bTABLE|\bDESCRIBE|\bDESC|\bTRUNCATE|\bINDEX\s+\w+\s+ON)\s+(?:[\w`.]+(?:\s+(?:AS\s+)?\w+)?\s*,\s*)*[\w`.]*$/i;
// the tables a statement names: the comma list after FROM, each JOIN, UPDATE and INTO
const FROM_LIST = /\bFROM\s+([^;()]*?)(?=\b(?:WHERE|GROUP|ORDER|LIMIT|HAVING|UNION|WINDOW|FOR|ON|USING)\b|\b(?:JOIN|LEFT|RIGHT|INNER|CROSS|FULL|NATURAL)\b|[;()]|$)/gi;
const ONE_TABLE = /^`?([\w.]+)`?(?:\s+(?:AS\s+)?`?(\w+)`?)?$/i;
const JOIN_OR_WRITE = /\b(?:JOIN|UPDATE|INTO)\s+`?([\w.]+)`?(?:\s+(?:AS\s+)?`?(\w+)`?)?/gi;
const RESERVED_AFTER_TABLE = new Set([
  "WHERE", "SET", "ON", "USING", "JOIN", "LEFT", "RIGHT", "INNER", "CROSS", "FULL", "NATURAL", "GROUP", "ORDER", "LIMIT", "HAVING", "UNION", "VALUES", "SELECT",
  "FOR", "WINDOW", "AS", "PARTITION", "OFFSET",
]);

let tablesCache: { at: number; value: string[] } | null = null;
const columnsCache = new Map<string, { at: number; value: ColumnDetail[] }>();
const registered = new WeakSet<object>();

export function clearSqlCompletionCache() {
  tablesCache = null;
  columnsCache.clear();
}

async function tablesOfDatabase(): Promise<string[]> {
  if (tablesCache && Date.now() - tablesCache.at < TTL_MS) return tablesCache.value;
  try {
    const value = await invoke<string[]>("get_tables");
    tablesCache = { at: Date.now(), value };
    return value;
  } catch {
    return tablesCache?.value ?? [];
  }
}

async function columnsOf(table: string): Promise<ColumnDetail[]> {
  const key = table.toLowerCase();
  const hit = columnsCache.get(key);
  if (hit && Date.now() - hit.at < TTL_MS) return hit.value;
  try {
    const value = await invoke<ColumnDetail[]>("get_columns_detail", { table });
    columnsCache.set(key, { at: Date.now(), value });
    return value;
  } catch {
    return hit?.value ?? [];
  }
}

// the statement the cursor is in: the text between the semicolons around it, up to the cursor
function statementAround(model: Monaco.editor.ITextModel, position: Monaco.Position): { before: string; whole: string } {
  const text = model.getValue();
  const offset = model.getOffsetAt(position);
  const start = text.lastIndexOf(";", Math.max(0, offset - 1)) + 1;
  const endAt = text.indexOf(";", offset);
  const end = endAt === -1 ? text.length : endAt;
  return { before: text.slice(start, offset), whole: text.slice(start, end) };
}

type Named = { table: string; alias: string };

function named(name: string, alias: string | undefined): Named {
  return {
    table: name.includes(".") ? name.slice(name.lastIndexOf(".") + 1) : name,
    alias: alias && !RESERVED_AFTER_TABLE.has(alias.toUpperCase()) ? alias : "",
  };
}

function namedTables(statement: string): Named[] {
  const out: Named[] = [];
  FROM_LIST.lastIndex = 0;
  let m: RegExpExecArray | null;
  while ((m = FROM_LIST.exec(statement))) {
    for (const item of m[1].split(",")) {
      const one = ONE_TABLE.exec(item.trim());
      if (one) out.push(named(one[1], one[2]));
    }
    if (m[0].length === 0) FROM_LIST.lastIndex++;
  }
  JOIN_OR_WRITE.lastIndex = 0;
  while ((m = JOIN_OR_WRITE.exec(statement))) out.push(named(m[1], m[2]));
  return out;
}

export function registerSqlCompletion(monaco: typeof Monaco) {
  if (registered.has(monaco)) return;
  registered.add(monaco);
  const kinds = monaco.languages.CompletionItemKind;

  monaco.languages.registerCompletionItemProvider("sql", {
    triggerCharacters: ["."],
    provideCompletionItems: async (model, position) => {
      const word = model.getWordUntilPosition(position);
      const range = new monaco.Range(position.lineNumber, word.startColumn, position.lineNumber, word.endColumn);
      const { before, whole } = statementAround(model, position);
      const named = namedTables(whole);

      const suggestions: Monaco.languages.CompletionItem[] = [];
      const columnItems = (table: string, cols: ColumnDetail[], rank: string) =>
        cols.map<Monaco.languages.CompletionItem>(c => ({
          label: c.name,
          kind: c.is_pk ? kinds.Constant : kinds.Field,
          detail: `${table} · ${c.data_type}${c.is_pk ? " · PK" : ""}${c.fk_ref ? " → " + c.fk_ref : ""}`,
          insertText: c.name,
          sortText: rank + c.name,
          range,
        }));

      // `alias.` / `table.`: the columns of that table only
      const qualifier = /([\w`]+)\.\s*\w*$/.exec(before);
      if (qualifier) {
        const q = qualifier[1].replace(/`/g, "").toLowerCase();
        const hit = named.find(n => n.alias.toLowerCase() === q) ?? named.find(n => n.table.toLowerCase() === q);
        const table = hit ? hit.table : q;
        return { suggestions: columnItems(table, await columnsOf(table), "0") };
      }

      const tables = await tablesOfDatabase();
      const tableItems = tables.map<Monaco.languages.CompletionItem>(t => ({
        label: t, kind: kinds.Struct, detail: "table", insertText: t, sortText: "1" + t, range,
      }));

      // after FROM / JOIN / INTO / UPDATE: tables
      if (TABLE_AFTER.test(before)) return { suggestions: tableItems.map(i => ({ ...i, sortText: "0" + i.label })) };

      // elsewhere: the columns of the tables the statement names, then tables, functions, keywords
      const seen = new Set<string>();
      for (const n of named) {
        const key = n.table.toLowerCase();
        if (seen.has(key) || !tables.some(t => t.toLowerCase() === key)) continue;
        seen.add(key);
        const real = tables.find(t => t.toLowerCase() === key) ?? n.table;
        suggestions.push(...columnItems(real, await columnsOf(real), "0"));
      }
      suggestions.push(...tableItems);
      for (const f of FUNCTIONS) {
        suggestions.push({
          label: f, kind: kinds.Function, detail: "function", insertText: `${f}($0)`,
          insertTextRules: monaco.languages.CompletionItemInsertTextRule.InsertAsSnippet, sortText: "2" + f, range,
        });
      }
      for (const k of KEYWORDS) {
        suggestions.push({ label: k, kind: kinds.Keyword, insertText: k, sortText: "3" + k, range });
      }
      return { suggestions };
    },
  });
}
