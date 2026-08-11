#pragma once

// JSON (de)serialization for the subset of ast.hpp types needed by schema persistence
// (DataType, FkAction, ForeignKey, ColumnDef). Mirrors serde_json's default "externally
// tagged" representation for Rust enums: a unit variant serializes as a bare JSON
// string ("Int"), a variant carrying data serializes as a single-key object
// ({"Varchar": 20}, {"Decimal": [10, 2]}).
//
// Full Statement/ArithExpr/CondExpr JSON support is deferred to the phase that actually
// persists procedures/views/triggers (see migration plan) — only the schema-relevant
// subset is implemented here for Phase 3.

#include "engine/parser/ast.hpp"
#include "engine/json_support.hpp"

namespace engine {

void to_json(nlohmann::json& j, const DataType& dt);
void from_json(const nlohmann::json& j, DataType& dt);

void to_json(nlohmann::json& j, const FkAction& action);
void from_json(const nlohmann::json& j, FkAction& action);

void to_json(nlohmann::json& j, const ForeignKey& fk);
void from_json(const nlohmann::json& j, ForeignKey& fk);

void to_json(nlohmann::json& j, const ColumnDef& col);
void from_json(const nlohmann::json& j, ColumnDef& col);

void to_json(nlohmann::json& j, const PartitionKind& kind);
void from_json(const nlohmann::json& j, PartitionKind& kind);
void to_json(nlohmann::json& j, const PartitionDef& def);
void from_json(const nlohmann::json& j, PartitionDef& def);
void to_json(nlohmann::json& j, const PartitionBy& pb);
void from_json(const nlohmann::json& j, PartitionBy& pb);

// Needed by CREATE FUNCTION (its RETURN expression is JSON-serialized into
// Statement::CreateFunction::body at parse time, mirroring
// `serde_json::to_string(&expr)` in parser.rs's parse_create_function).
void to_json(nlohmann::json& j, const ArithExpr& expr);
void from_json(const nlohmann::json& j, ArithExpr& expr);

// ---------------------------------------------------------------------------
// Full Statement (de)serialization, needed by Phase 8's SharedDatabase to
// persist views/procedures/triggers as JSON (DiskManager::save_views etc.),
// exactly mirroring the fact that Statement derives Serialize/Deserialize in
// the Rust original. NOTE: cross-language binary/file compatibility with the
// Rust build is explicitly out of scope for this migration (see plan), so
// these only need to round-trip correctly within this C++ build; JSON object
// keys use the C++ field names (e.g. "op" not "operator") rather than
// reproducing serde's exact wire format.
void to_json(nlohmann::json& j, const IsolationLevel& lvl);
void from_json(const nlohmann::json& j, IsolationLevel& lvl);

void to_json(nlohmann::json& j, const JoinType& jt);
void from_json(const nlohmann::json& j, JoinType& jt);

void to_json(nlohmann::json& j, const Operator& op);
void from_json(const nlohmann::json& j, Operator& op);

void to_json(nlohmann::json& j, const TriggerTiming& t);
void from_json(const nlohmann::json& j, TriggerTiming& t);

void to_json(nlohmann::json& j, const TriggerEvent& e);
void from_json(const nlohmann::json& j, TriggerEvent& e);

void to_json(nlohmann::json& j, const Join& join);
void from_json(const nlohmann::json& j, Join& join);

void to_json(nlohmann::json& j, const OrderBy& ob);
void from_json(const nlohmann::json& j, OrderBy& ob);

void to_json(nlohmann::json& j, const CaseWhenBranch& b);
void from_json(const nlohmann::json& j, CaseWhenBranch& b);

void to_json(nlohmann::json& j, const AggFunc& f);
void from_json(const nlohmann::json& j, AggFunc& f);

void to_json(nlohmann::json& j, const FrameBound& fb);
void from_json(const nlohmann::json& j, FrameBound& fb);

void to_json(nlohmann::json& j, const FrameUnit& u);
void from_json(const nlohmann::json& j, FrameUnit& u);

void to_json(nlohmann::json& j, const WindowFrame& wf);
void from_json(const nlohmann::json& j, WindowFrame& wf);

void to_json(nlohmann::json& j, const WindowFunc& f);
void from_json(const nlohmann::json& j, WindowFunc& f);

void to_json(nlohmann::json& j, const InsertConflict& ic);
void from_json(const nlohmann::json& j, InsertConflict& ic);

void to_json(nlohmann::json& j, const Condition& c);
void from_json(const nlohmann::json& j, Condition& c);

void to_json(nlohmann::json& j, const ConditionValue& cv);
void from_json(const nlohmann::json& j, ConditionValue& cv);

void to_json(nlohmann::json& j, const CondExpr& expr);
void from_json(const nlohmann::json& j, CondExpr& expr);

void to_json(nlohmann::json& j, const SelectColumn& col);
void from_json(const nlohmann::json& j, SelectColumn& col);

void to_json(nlohmann::json& j, const AlterAction& action);
void from_json(const nlohmann::json& j, AlterAction& action);

void to_json(nlohmann::json& j, const Statement& stmt);
void from_json(const nlohmann::json& j, Statement& stmt);

} // namespace engine
