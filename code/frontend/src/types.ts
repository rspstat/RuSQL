// types.ts
// Shared types used across App.tsx and the extracted presentational components
// (Sidebar, ErdView, context menus, modals). Moved out of App.tsx unchanged during
// the App.tsx UI-section extraction refactor -- no behavior change.

export interface QueryResult {
  columns: string[];
  rows: string[][];
  message: string;
  elapsed: number;
  success: boolean;
}
export interface MultiQueryResult {
  results: QueryResult[];
  total_elapsed: number;
}
export interface ServerStatus {
  running: boolean;
  port: number;
  client_count: number;
  log: string[];
  sessions: { addr: string; user: string; connected_at: number; query_count: number }[];
}
export interface IndexInfo {
  name: string;
  table: string;
  columns: string[];
  kind: "single" | "composite" | "hash";
}
export interface TriggerInfo {
  name: string;
  table: string;
  timing: string;
  event: string;
}
export interface ColumnDetail {
  name: string;
  data_type: string;
  is_pk: boolean;
  is_not_null: boolean;
  is_unique: boolean;
  is_auto_inc: boolean;
  default_val: string | null;
  fk_ref: string | null;
}

// DB별 Tables/Views/Indexes/Triggers 데이터 (사이드바)
export interface DbData { tables: string[]; views: string[]; indexes: IndexInfo[]; triggers: TriggerInfo[]; }
