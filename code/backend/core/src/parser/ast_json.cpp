#include "engine/parser/ast_json.hpp"

namespace engine {

void to_json(nlohmann::json& j, const DataType& dt) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int>) j = "Int";
            else if constexpr (std::is_same_v<T, DataType::BigInt>) j = "BigInt";
            else if constexpr (std::is_same_v<T, DataType::SmallInt>) j = "SmallInt";
            else if constexpr (std::is_same_v<T, DataType::TinyInt>) j = "TinyInt";
            else if constexpr (std::is_same_v<T, DataType::Text>) j = "Text";
            else if constexpr (std::is_same_v<T, DataType::Float>) j = "Float";
            else if constexpr (std::is_same_v<T, DataType::Boolean>) j = "Boolean";
            else if constexpr (std::is_same_v<T, DataType::Varchar>) {
                nlohmann::json obj; obj["Varchar"] = alt.length; j = obj;
            } else if constexpr (std::is_same_v<T, DataType::Date>) j = "Date";
            else if constexpr (std::is_same_v<T, DataType::DateTime>) j = "DateTime";
            else if constexpr (std::is_same_v<T, DataType::Timestamp>) j = "Timestamp";
            else if constexpr (std::is_same_v<T, DataType::Decimal>) {
                nlohmann::json obj;
                obj["Decimal"] = nlohmann::json::array({alt.precision, alt.scale});
                j = obj;
            } else if constexpr (std::is_same_v<T, DataType::Double>) j = "Double";
            else if constexpr (std::is_same_v<T, DataType::Time>) j = "Time";
            else if constexpr (std::is_same_v<T, DataType::Year>) j = "Year";
            else if constexpr (std::is_same_v<T, DataType::Enum>) {
                nlohmann::json obj; obj["Enum"] = alt.values; j = obj;
            } else if constexpr (std::is_same_v<T, DataType::Set>) {
                nlohmann::json obj; obj["Set"] = alt.values; j = obj;
            } else if constexpr (std::is_same_v<T, DataType::Blob>) j = "Blob";
            else if constexpr (std::is_same_v<T, DataType::Json>) j = "Json";
            else if constexpr (std::is_same_v<T, DataType::Unknown>) j = "Unknown";
        },
        dt.data);
}

void from_json(const nlohmann::json& j, DataType& dt) {
    if (j.is_string()) {
        const std::string tag = j.get<std::string>();
        if (tag == "Int") dt = DataType::Int{};
        else if (tag == "BigInt") dt = DataType::BigInt{};
        else if (tag == "SmallInt") dt = DataType::SmallInt{};
        else if (tag == "TinyInt") dt = DataType::TinyInt{};
        else if (tag == "Text") dt = DataType::Text{};
        else if (tag == "Float") dt = DataType::Float{};
        else if (tag == "Boolean") dt = DataType::Boolean{};
        else if (tag == "Date") dt = DataType::Date{};
        else if (tag == "DateTime") dt = DataType::DateTime{};
        else if (tag == "Timestamp") dt = DataType::Timestamp{};
        else if (tag == "Double") dt = DataType::Double{};
        else if (tag == "Time") dt = DataType::Time{};
        else if (tag == "Year") dt = DataType::Year{};
        else if (tag == "Blob") dt = DataType::Blob{};
        else if (tag == "Json") dt = DataType::Json{};
        else dt = DataType::Unknown{}; // serde(other) fallback
    } else if (j.is_object() && !j.empty()) {
        auto it = j.begin();
        const std::string& tag = it.key();
        const auto& payload = it.value();
        if (tag == "Varchar") dt = DataType::Varchar{payload.get<std::uint32_t>()};
        else if (tag == "Decimal") dt = DataType::Decimal{payload.at(0).get<std::uint8_t>(), payload.at(1).get<std::uint8_t>()};
        else if (tag == "Enum") dt = DataType::Enum{payload.get<std::vector<std::string>>()};
        else if (tag == "Set") dt = DataType::Set{payload.get<std::vector<std::string>>()};
        else dt = DataType::Unknown{};
    } else {
        dt = DataType::Unknown{};
    }
}

void to_json(nlohmann::json& j, const FkAction& action) {
    switch (action) {
        case FkAction::Restrict:   j = "Restrict"; break;
        case FkAction::Cascade:    j = "Cascade"; break;
        case FkAction::SetNull:    j = "SetNull"; break;
        case FkAction::SetDefault: j = "SetDefault"; break;
    }
}

void from_json(const nlohmann::json& j, FkAction& action) {
    const std::string tag = j.get<std::string>();
    if (tag == "Cascade") action = FkAction::Cascade;
    else if (tag == "SetNull") action = FkAction::SetNull;
    else if (tag == "SetDefault") action = FkAction::SetDefault;
    else action = FkAction::Restrict; // default, matches Rust's `impl Default for FkAction`
}

void to_json(nlohmann::json& j, const ForeignKey& fk) {
    j = nlohmann::json{
        {"column", fk.column},
        {"ref_table", fk.ref_table},
        {"ref_column", fk.ref_column},
        {"on_delete", fk.on_delete},
        {"on_update", fk.on_update},
    };
}

void from_json(const nlohmann::json& j, ForeignKey& fk) {
    j.at("column").get_to(fk.column);
    j.at("ref_table").get_to(fk.ref_table);
    j.at("ref_column").get_to(fk.ref_column);
    j.at("on_delete").get_to(fk.on_delete);
    // #[serde(default)] on ForeignKey::on_update in the schema-persisted variant
    if (j.contains("on_update")) j.at("on_update").get_to(fk.on_update);
    else fk.on_update = FkAction::Restrict;
}

void to_json(nlohmann::json& j, const ColumnDef& col) {
    j = nlohmann::json{
        {"name", col.name},
        {"data_type", col.data_type},
        {"primary_key", col.primary_key},
        {"not_null", col.not_null},
        {"unique", col.unique},
        {"unique_constraint_name", col.unique_constraint_name},
        {"auto_increment", col.auto_increment},
        {"default", col.default_value}, // Rust field name is `default`
        {"foreign_key", col.foreign_key},
        {"check_expr", col.check_expr},
    };
}

void from_json(const nlohmann::json& j, ColumnDef& col) {
    j.at("name").get_to(col.name);
    j.at("data_type").get_to(col.data_type);
    j.at("primary_key").get_to(col.primary_key);
    j.at("not_null").get_to(col.not_null);
    j.at("unique").get_to(col.unique);
    j.at("unique_constraint_name").get_to(col.unique_constraint_name);
    j.at("auto_increment").get_to(col.auto_increment);
    if (j.contains("default")) j.at("default").get_to(col.default_value);
    j.at("foreign_key").get_to(col.foreign_key);
    if (j.contains("check_expr")) j.at("check_expr").get_to(col.check_expr);
    else col.check_expr = std::nullopt;
}

void to_json(nlohmann::json& j, const PartitionKind& kind) {
    switch (kind) {
        case PartitionKind::Range: j = "Range"; break;
        case PartitionKind::List: j = "List"; break;
        case PartitionKind::Hash: j = "Hash"; break;
    }
}

void from_json(const nlohmann::json& j, PartitionKind& kind) {
    const std::string tag = j.get<std::string>();
    if (tag == "List") kind = PartitionKind::List;
    else if (tag == "Hash") kind = PartitionKind::Hash;
    else kind = PartitionKind::Range;
}

void to_json(nlohmann::json& j, const PartitionDef& def) {
    j = nlohmann::json{
        {"name", def.name},
        {"range_upper_bound", def.range_upper_bound},
        {"range_is_maxvalue", def.range_is_maxvalue},
        {"list_values", def.list_values},
        {"child_table", def.child_table},
    };
}

void from_json(const nlohmann::json& j, PartitionDef& def) {
    j.at("name").get_to(def.name);
    j.at("range_upper_bound").get_to(def.range_upper_bound);
    j.at("range_is_maxvalue").get_to(def.range_is_maxvalue);
    j.at("list_values").get_to(def.list_values);
    j.at("child_table").get_to(def.child_table);
}

void to_json(nlohmann::json& j, const PartitionBy& pb) {
    j = nlohmann::json{
        {"kind", pb.kind},
        {"column", pb.column},
        {"partitions", pb.partitions},
        {"hash_partitions", pb.hash_partitions},
    };
}

void from_json(const nlohmann::json& j, PartitionBy& pb) {
    j.at("kind").get_to(pb.kind);
    j.at("column").get_to(pb.column);
    j.at("partitions").get_to(pb.partitions);
    j.at("hash_partitions").get_to(pb.hash_partitions);
}

void to_json(nlohmann::json& j, const ArithExpr& expr) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            nlohmann::json obj;
            if constexpr (std::is_same_v<T, ArithExpr::Col>) obj["Col"] = alt.name;
            else if constexpr (std::is_same_v<T, ArithExpr::Num>) obj["Num"] = alt.value;
            else if constexpr (std::is_same_v<T, ArithExpr::Str>) obj["Str"] = alt.value;
            else if constexpr (std::is_same_v<T, ArithExpr::Add>) obj["Add"] = nlohmann::json::array({*alt.lhs, *alt.rhs});
            else if constexpr (std::is_same_v<T, ArithExpr::Sub>) obj["Sub"] = nlohmann::json::array({*alt.lhs, *alt.rhs});
            else if constexpr (std::is_same_v<T, ArithExpr::Mul>) obj["Mul"] = nlohmann::json::array({*alt.lhs, *alt.rhs});
            else if constexpr (std::is_same_v<T, ArithExpr::Div>) obj["Div"] = nlohmann::json::array({*alt.lhs, *alt.rhs});
            else if constexpr (std::is_same_v<T, ArithExpr::Func>) obj["Func"] = nlohmann::json::array({alt.name, alt.args});
            else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) obj["Cmp"] = nlohmann::json::array({*alt.lhs, alt.op, *alt.rhs});
            j = obj;
        },
        expr.data);
}

void from_json(const nlohmann::json& j, ArithExpr& expr) {
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid ArithExpr JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& payload = it.value();
    if (tag == "Col") expr = ArithExpr(ArithExpr::Col{payload.get<std::string>()});
    else if (tag == "Num") expr = ArithExpr(ArithExpr::Num{payload.get<std::string>()});
    else if (tag == "Str") expr = ArithExpr(ArithExpr::Str{payload.get<std::string>()});
    else if (tag == "Add") expr = ArithExpr(ArithExpr::Add{std::make_unique<ArithExpr>(payload.at(0).get<ArithExpr>()), std::make_unique<ArithExpr>(payload.at(1).get<ArithExpr>())});
    else if (tag == "Sub") expr = ArithExpr(ArithExpr::Sub{std::make_unique<ArithExpr>(payload.at(0).get<ArithExpr>()), std::make_unique<ArithExpr>(payload.at(1).get<ArithExpr>())});
    else if (tag == "Mul") expr = ArithExpr(ArithExpr::Mul{std::make_unique<ArithExpr>(payload.at(0).get<ArithExpr>()), std::make_unique<ArithExpr>(payload.at(1).get<ArithExpr>())});
    else if (tag == "Div") expr = ArithExpr(ArithExpr::Div{std::make_unique<ArithExpr>(payload.at(0).get<ArithExpr>()), std::make_unique<ArithExpr>(payload.at(1).get<ArithExpr>())});
    else if (tag == "Func") expr = ArithExpr(ArithExpr::Func{payload.at(0).get<std::string>(), payload.at(1).get<std::vector<ArithExpr>>()});
    else if (tag == "Cmp") expr = ArithExpr(ArithExpr::Cmp{std::make_unique<ArithExpr>(payload.at(0).get<ArithExpr>()), payload.at(1).get<std::string>(), std::make_unique<ArithExpr>(payload.at(2).get<ArithExpr>())});
    else throw std::runtime_error("unknown ArithExpr tag: " + tag);
}

void to_json(nlohmann::json& j, const IsolationLevel& lvl) {
    switch (lvl) {
        case IsolationLevel::ReadUncommitted: j = "ReadUncommitted"; break;
        case IsolationLevel::ReadCommitted:   j = "ReadCommitted"; break;
        case IsolationLevel::RepeatableRead:  j = "RepeatableRead"; break;
        case IsolationLevel::Serializable:    j = "Serializable"; break;
    }
}

void from_json(const nlohmann::json& j, IsolationLevel& lvl) {
    const std::string tag = j.get<std::string>();
    if (tag == "ReadUncommitted") lvl = IsolationLevel::ReadUncommitted;
    else if (tag == "RepeatableRead") lvl = IsolationLevel::RepeatableRead;
    else if (tag == "Serializable") lvl = IsolationLevel::Serializable;
    else lvl = IsolationLevel::ReadCommitted;
}

void to_json(nlohmann::json& j, const JoinType& jt) {
    switch (jt) {
        case JoinType::Inner: j = "Inner"; break;
        case JoinType::Left: j = "Left"; break;
        case JoinType::Right: j = "Right"; break;
        case JoinType::Cross: j = "Cross"; break;
        case JoinType::Natural: j = "Natural"; break;
        case JoinType::FullOuter: j = "FullOuter"; break;
    }
}

void from_json(const nlohmann::json& j, JoinType& jt) {
    const std::string tag = j.get<std::string>();
    if (tag == "Left") jt = JoinType::Left;
    else if (tag == "Right") jt = JoinType::Right;
    else if (tag == "Cross") jt = JoinType::Cross;
    else if (tag == "Natural") jt = JoinType::Natural;
    else if (tag == "FullOuter") jt = JoinType::FullOuter;
    else jt = JoinType::Inner;
}

void to_json(nlohmann::json& j, const Operator& op) {
    switch (op) {
        case Operator::Eq: j = "Eq"; break;
        case Operator::Ne: j = "Ne"; break;
        case Operator::Gt: j = "Gt"; break;
        case Operator::Lt: j = "Lt"; break;
        case Operator::Gte: j = "Gte"; break;
        case Operator::Lte: j = "Lte"; break;
        case Operator::In: j = "In"; break;
        case Operator::NotIn: j = "NotIn"; break;
        case Operator::Like: j = "Like"; break;
        case Operator::NotLike: j = "NotLike"; break;
        case Operator::Between: j = "Between"; break;
        case Operator::NotBetween: j = "NotBetween"; break;
        case Operator::IsNull: j = "IsNull"; break;
        case Operator::IsNotNull: j = "IsNotNull"; break;
        case Operator::Exists: j = "Exists"; break;
        case Operator::NotExists: j = "NotExists"; break;
        case Operator::Regexp: j = "Regexp"; break;
        case Operator::NotRegexp: j = "NotRegexp"; break;
    }
}

void from_json(const nlohmann::json& j, Operator& op) {
    const std::string tag = j.get<std::string>();
    if (tag == "Ne") op = Operator::Ne;
    else if (tag == "Gt") op = Operator::Gt;
    else if (tag == "Lt") op = Operator::Lt;
    else if (tag == "Gte") op = Operator::Gte;
    else if (tag == "Lte") op = Operator::Lte;
    else if (tag == "In") op = Operator::In;
    else if (tag == "NotIn") op = Operator::NotIn;
    else if (tag == "Like") op = Operator::Like;
    else if (tag == "NotLike") op = Operator::NotLike;
    else if (tag == "Between") op = Operator::Between;
    else if (tag == "NotBetween") op = Operator::NotBetween;
    else if (tag == "IsNull") op = Operator::IsNull;
    else if (tag == "IsNotNull") op = Operator::IsNotNull;
    else if (tag == "Exists") op = Operator::Exists;
    else if (tag == "NotExists") op = Operator::NotExists;
    else if (tag == "Regexp") op = Operator::Regexp;
    else if (tag == "NotRegexp") op = Operator::NotRegexp;
    else op = Operator::Eq;
}

void to_json(nlohmann::json& j, const TriggerTiming& t) {
    j = (t == TriggerTiming::Before) ? "Before" : "After";
}

void from_json(const nlohmann::json& j, TriggerTiming& t) {
    t = (j.get<std::string>() == "After") ? TriggerTiming::After : TriggerTiming::Before;
}

void to_json(nlohmann::json& j, const TriggerEvent& e) {
    switch (e) {
        case TriggerEvent::Insert: j = "Insert"; break;
        case TriggerEvent::Update: j = "Update"; break;
        case TriggerEvent::Delete: j = "Delete"; break;
    }
}

void from_json(const nlohmann::json& j, TriggerEvent& e) {
    const std::string tag = j.get<std::string>();
    if (tag == "Update") e = TriggerEvent::Update;
    else if (tag == "Delete") e = TriggerEvent::Delete;
    else e = TriggerEvent::Insert;
}

void to_json(nlohmann::json& j, const Join& join) {
    // LATERAL JOIN -- Rust 원본에 없음. subquery는 Select::subquery(769/1011-1013번 줄)와
    // 동일한 [stmt, alias] 2-원소 배열 관례를 그대로 따른다.
    nlohmann::json sub = nullptr;
    if (join.subquery) sub = nlohmann::json::array({*join.subquery->first, join.subquery->second});
    j = nlohmann::json{{"table", join.table}, {"on_expr", join.on_expr},
                        {"join_type", join.join_type}, {"using_cols", join.using_cols},
                        {"subquery", sub}, {"lateral", join.lateral}};
}

void from_json(const nlohmann::json& j, Join& join) {
    j.at("table").get_to(join.table);
    j.at("on_expr").get_to(join.on_expr);
    j.at("join_type").get_to(join.join_type);
    j.at("using_cols").get_to(join.using_cols);
    join.lateral = j.contains("lateral") && j.at("lateral").get<bool>();
    if (j.contains("subquery") && !j.at("subquery").is_null()) {
        auto sub_j = j.at("subquery");
        join.subquery = std::make_pair(std::make_unique<Statement>(sub_j.at(0).get<Statement>()), sub_j.at(1).get<std::string>());
    }
}

void to_json(nlohmann::json& j, const OrderBy& ob) {
    j = nlohmann::json{{"column", ob.column}, {"ascending", ob.ascending}};
}

void from_json(const nlohmann::json& j, OrderBy& ob) {
    j.at("column").get_to(ob.column);
    j.at("ascending").get_to(ob.ascending);
}

void to_json(nlohmann::json& j, const CaseWhenBranch& b) {
    j = nlohmann::json{{"condition", b.condition}, {"result", b.result}};
}

void from_json(const nlohmann::json& j, CaseWhenBranch& b) {
    j.at("condition").get_to(b.condition);
    j.at("result").get_to(b.result);
}

void to_json(nlohmann::json& j, const AggFunc& f) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AggFunc::Count>) j = "Count";
            else if constexpr (std::is_same_v<T, AggFunc::CountDistinct>) j = "CountDistinct";
            else if constexpr (std::is_same_v<T, AggFunc::Sum>) j = "Sum";
            else if constexpr (std::is_same_v<T, AggFunc::Avg>) j = "Avg";
            else if constexpr (std::is_same_v<T, AggFunc::Min>) j = "Min";
            else if constexpr (std::is_same_v<T, AggFunc::Max>) j = "Max";
            else if constexpr (std::is_same_v<T, AggFunc::SumDistinct>) j = "SumDistinct";
            else if constexpr (std::is_same_v<T, AggFunc::AvgDistinct>) j = "AvgDistinct";
            else if constexpr (std::is_same_v<T, AggFunc::Stddev>) j = "Stddev";
            else if constexpr (std::is_same_v<T, AggFunc::Variance>) j = "Variance";
            else if constexpr (std::is_same_v<T, AggFunc::GroupConcat>) j = nlohmann::json{{"GroupConcat", alt.separator}};
            else if constexpr (std::is_same_v<T, AggFunc::CountCase>)
                j = nlohmann::json{{"CountCase", nlohmann::json{{"branches", alt.branches}, {"else_val", alt.else_val}}}};
            else if constexpr (std::is_same_v<T, AggFunc::SumCase>)
                j = nlohmann::json{{"SumCase", nlohmann::json{{"branches", alt.branches}, {"else_val", alt.else_val}}}};
            else if constexpr (std::is_same_v<T, AggFunc::BitAnd>) j = "BitAnd";
            else if constexpr (std::is_same_v<T, AggFunc::BitOr>) j = "BitOr";
            else if constexpr (std::is_same_v<T, AggFunc::JsonAgg>) j = "JsonAgg";
        },
        f.data);
}

void from_json(const nlohmann::json& j, AggFunc& f) {
    if (j.is_string()) {
        const std::string tag = j.get<std::string>();
        if (tag == "Count") f = AggFunc::Count{};
        else if (tag == "CountDistinct") f = AggFunc::CountDistinct{};
        else if (tag == "Sum") f = AggFunc::Sum{};
        else if (tag == "Avg") f = AggFunc::Avg{};
        else if (tag == "Min") f = AggFunc::Min{};
        else if (tag == "Max") f = AggFunc::Max{};
        else if (tag == "SumDistinct") f = AggFunc::SumDistinct{};
        else if (tag == "AvgDistinct") f = AggFunc::AvgDistinct{};
        else if (tag == "Stddev") f = AggFunc::Stddev{};
        else if (tag == "Variance") f = AggFunc::Variance{};
        else if (tag == "BitAnd") f = AggFunc::BitAnd{};
        else if (tag == "BitOr") f = AggFunc::BitOr{};
        else if (tag == "JsonAgg") f = AggFunc::JsonAgg{};
        else throw std::runtime_error("unknown AggFunc tag: " + tag);
        return;
    }
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid AggFunc JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();
    if (tag == "GroupConcat") f = AggFunc::GroupConcat{p.get<std::string>()};
    else if (tag == "CountCase")
        f = AggFunc::CountCase{p.at("branches").get<std::vector<CaseWhenBranch>>(), p.at("else_val").get<std::optional<std::string>>()};
    else if (tag == "SumCase")
        f = AggFunc::SumCase{p.at("branches").get<std::vector<CaseWhenBranch>>(), p.at("else_val").get<std::optional<std::string>>()};
    else throw std::runtime_error("unknown AggFunc tag: " + tag);
}

void to_json(nlohmann::json& j, const FrameBound& fb) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, FrameBound::UnboundedPreceding>) j = "UnboundedPreceding";
            else if constexpr (std::is_same_v<T, FrameBound::Preceding>) j = nlohmann::json{{"Preceding", alt.n}};
            else if constexpr (std::is_same_v<T, FrameBound::CurrentRow>) j = "CurrentRow";
            else if constexpr (std::is_same_v<T, FrameBound::Following>) j = nlohmann::json{{"Following", alt.n}};
            else if constexpr (std::is_same_v<T, FrameBound::UnboundedFollowing>) j = "UnboundedFollowing";
        },
        fb.data);
}

void from_json(const nlohmann::json& j, FrameBound& fb) {
    if (j.is_string()) {
        const std::string tag = j.get<std::string>();
        if (tag == "UnboundedPreceding") fb = FrameBound::UnboundedPreceding{};
        else if (tag == "CurrentRow") fb = FrameBound::CurrentRow{};
        else if (tag == "UnboundedFollowing") fb = FrameBound::UnboundedFollowing{};
        else throw std::runtime_error("unknown FrameBound tag: " + tag);
        return;
    }
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid FrameBound JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();
    if (tag == "Preceding") fb = FrameBound::Preceding{p.get<std::size_t>()};
    else if (tag == "Following") fb = FrameBound::Following{p.get<std::size_t>()};
    else throw std::runtime_error("unknown FrameBound tag: " + tag);
}

void to_json(nlohmann::json& j, const FrameUnit& u) {
    j = (u == FrameUnit::Rows) ? "Rows" : "Range";
}

void from_json(const nlohmann::json& j, FrameUnit& u) {
    u = (j.get<std::string>() == "Range") ? FrameUnit::Range : FrameUnit::Rows;
}

void to_json(nlohmann::json& j, const WindowFrame& wf) {
    j = nlohmann::json{{"unit", wf.unit}, {"start", wf.start}, {"end", wf.end}};
}

void from_json(const nlohmann::json& j, WindowFrame& wf) {
    j.at("unit").get_to(wf.unit);
    j.at("start").get_to(wf.start);
    j.at("end").get_to(wf.end);
}

void to_json(nlohmann::json& j, const WindowFunc& f) {
    switch (f) {
        case WindowFunc::RowNumber: j = "RowNumber"; break;
        case WindowFunc::Rank: j = "Rank"; break;
        case WindowFunc::DenseRank: j = "DenseRank"; break;
        case WindowFunc::Lag: j = "Lag"; break;
        case WindowFunc::Lead: j = "Lead"; break;
        case WindowFunc::FirstValue: j = "FirstValue"; break;
        case WindowFunc::LastValue: j = "LastValue"; break;
        case WindowFunc::NthValue: j = "NthValue"; break;
        case WindowFunc::Ntile: j = "Ntile"; break;
        case WindowFunc::PercentRank: j = "PercentRank"; break;
        case WindowFunc::CumeDist: j = "CumeDist"; break;
        case WindowFunc::Sum: j = "Sum"; break;
        case WindowFunc::Avg: j = "Avg"; break;
        case WindowFunc::Count: j = "Count"; break;
        case WindowFunc::Min: j = "Min"; break;
        case WindowFunc::Max: j = "Max"; break;
    }
}

void from_json(const nlohmann::json& j, WindowFunc& f) {
    const std::string tag = j.get<std::string>();
    if (tag == "RowNumber") f = WindowFunc::RowNumber;
    else if (tag == "Rank") f = WindowFunc::Rank;
    else if (tag == "DenseRank") f = WindowFunc::DenseRank;
    else if (tag == "Lag") f = WindowFunc::Lag;
    else if (tag == "Lead") f = WindowFunc::Lead;
    else if (tag == "FirstValue") f = WindowFunc::FirstValue;
    else if (tag == "LastValue") f = WindowFunc::LastValue;
    else if (tag == "NthValue") f = WindowFunc::NthValue;
    else if (tag == "Ntile") f = WindowFunc::Ntile;
    else if (tag == "PercentRank") f = WindowFunc::PercentRank;
    else if (tag == "CumeDist") f = WindowFunc::CumeDist;
    else if (tag == "Sum") f = WindowFunc::Sum;
    else if (tag == "Avg") f = WindowFunc::Avg;
    else if (tag == "Count") f = WindowFunc::Count;
    else if (tag == "Min") f = WindowFunc::Min;
    else if (tag == "Max") f = WindowFunc::Max;
    else throw std::runtime_error("unknown WindowFunc tag: " + tag);
}

void to_json(nlohmann::json& j, const InsertConflict& ic) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, InsertConflict::Abort>) j = "Abort";
            else if constexpr (std::is_same_v<T, InsertConflict::Ignore>) j = "Ignore";
            else if constexpr (std::is_same_v<T, InsertConflict::Update>) j = nlohmann::json{{"Update", alt.assignments}};
        },
        ic.data);
}

void from_json(const nlohmann::json& j, InsertConflict& ic) {
    if (j.is_string()) {
        ic = (j.get<std::string>() == "Ignore") ? InsertConflict(InsertConflict::Ignore{}) : InsertConflict(InsertConflict::Abort{});
        return;
    }
    if (j.is_object() && !j.empty() && j.begin().key() == "Update") {
        ic = InsertConflict::Update{j.begin().value().get<std::vector<std::pair<std::string, ArithExpr>>>()};
        return;
    }
    ic = InsertConflict::Abort{};
}

void to_json(nlohmann::json& j, const Condition& c) {
    j = nlohmann::json{{"left", c.left}, {"op", c.op}, {"value", c.value}};
}

void from_json(const nlohmann::json& j, Condition& c) {
    j.at("left").get_to(c.left);
    j.at("op").get_to(c.op);
    j.at("value").get_to(c.value);
}

void to_json(nlohmann::json& j, const ConditionValue& cv) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ConditionValue::Literal>) j = nlohmann::json{{"Literal", alt.value}};
            else if constexpr (std::is_same_v<T, ConditionValue::Subquery>) j = nlohmann::json{{"Subquery", *alt.query}};
            else if constexpr (std::is_same_v<T, ConditionValue::Between>) j = nlohmann::json{{"Between", nlohmann::json::array({alt.lo, alt.hi})}};
            else if constexpr (std::is_same_v<T, ConditionValue::LiteralList>) j = nlohmann::json{{"LiteralList", alt.values}};
            else if constexpr (std::is_same_v<T, ConditionValue::Arith>) j = nlohmann::json{{"Arith", alt.expr}};
        },
        cv.data);
}

void from_json(const nlohmann::json& j, ConditionValue& cv) {
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid ConditionValue JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();
    if (tag == "Literal") cv = ConditionValue(ConditionValue::Literal{p.get<std::string>()});
    else if (tag == "Subquery") cv = ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(p.get<Statement>())});
    else if (tag == "Between") cv = ConditionValue(ConditionValue::Between{p.at(0).get<std::string>(), p.at(1).get<std::string>()});
    else if (tag == "LiteralList") cv = ConditionValue(ConditionValue::LiteralList{p.get<std::vector<std::string>>()});
    else if (tag == "Arith") cv = ConditionValue(ConditionValue::Arith{p.get<ArithExpr>()});
    else throw std::runtime_error("unknown ConditionValue tag: " + tag);
}

void to_json(nlohmann::json& j, const CondExpr& expr) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, CondExpr::And>) j = nlohmann::json{{"And", nlohmann::json::array({*alt.lhs, *alt.rhs})}};
            else if constexpr (std::is_same_v<T, CondExpr::Or>) j = nlohmann::json{{"Or", nlohmann::json::array({*alt.lhs, *alt.rhs})}};
            else if constexpr (std::is_same_v<T, CondExpr::Not>) j = nlohmann::json{{"Not", *alt.inner}};
            else if constexpr (std::is_same_v<T, CondExpr::Leaf>) j = nlohmann::json{{"Leaf", alt.condition}};
        },
        expr.data);
}

void from_json(const nlohmann::json& j, CondExpr& expr) {
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid CondExpr JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();
    if (tag == "And")
        expr = CondExpr(CondExpr::And{std::make_unique<CondExpr>(p.at(0).get<CondExpr>()), std::make_unique<CondExpr>(p.at(1).get<CondExpr>())});
    else if (tag == "Or")
        expr = CondExpr(CondExpr::Or{std::make_unique<CondExpr>(p.at(0).get<CondExpr>()), std::make_unique<CondExpr>(p.at(1).get<CondExpr>())});
    else if (tag == "Not") expr = CondExpr(CondExpr::Not{std::make_unique<CondExpr>(p.get<CondExpr>())});
    else if (tag == "Leaf") expr = CondExpr(CondExpr::Leaf{p.get<Condition>()});
    else throw std::runtime_error("unknown CondExpr tag: " + tag);
}

void to_json(nlohmann::json& j, const SelectColumn& col) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, SelectColumn::All>) j = "All";
            else if constexpr (std::is_same_v<T, SelectColumn::Column>) j = nlohmann::json{{"Column", alt.name}};
            else if constexpr (std::is_same_v<T, SelectColumn::ColumnAlias>)
                j = nlohmann::json{{"ColumnAlias", nlohmann::json::array({alt.name, alt.alias})}};
            else if constexpr (std::is_same_v<T, SelectColumn::Agg>)
                j = nlohmann::json{{"Agg", nlohmann::json{{"func", alt.func}, {"col", alt.col}, {"filter", alt.filter}}}};
            else if constexpr (std::is_same_v<T, SelectColumn::AggAlias>)
                j = nlohmann::json{
                    {"AggAlias", nlohmann::json{{"func", alt.func}, {"col", alt.col}, {"alias", alt.alias}, {"filter", alt.filter}}}};
            else if constexpr (std::is_same_v<T, SelectColumn::Func>)
                j = nlohmann::json{{"Func", nlohmann::json{{"name", alt.name}, {"args", alt.args}, {"alias", alt.alias}}}};
            else if constexpr (std::is_same_v<T, SelectColumn::Expr>)
                j = nlohmann::json{{"Expr", nlohmann::json{{"expr", alt.expr}, {"alias", alt.alias}}}};
            else if constexpr (std::is_same_v<T, SelectColumn::CaseWhen>)
                j = nlohmann::json{{"CaseWhen", nlohmann::json{{"branches", alt.branches}, {"else_val", alt.else_val}, {"alias", alt.alias}}}};
            else if constexpr (std::is_same_v<T, SelectColumn::WinFunc>)
                j = nlohmann::json{{"WinFunc", nlohmann::json{{"func", alt.func},
                                                               {"col", alt.col},
                                                               {"offset", alt.offset},
                                                               {"partition_by", alt.partition_by},
                                                               {"order_by", alt.order_by},
                                                               {"alias", alt.alias},
                                                               {"frame", alt.frame}}}};
            else if constexpr (std::is_same_v<T, SelectColumn::Subquery>)
                j = nlohmann::json{{"Subquery", nlohmann::json{{"query", *alt.query}, {"alias", alt.alias}}}};
        },
        col.data);
}

void from_json(const nlohmann::json& j, SelectColumn& col) {
    if (j.is_string()) {
        col = SelectColumn::All{};
        return;
    }
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid SelectColumn JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();
    if (tag == "Column") col = SelectColumn(SelectColumn::Column{p.get<std::string>()});
    else if (tag == "ColumnAlias") col = SelectColumn(SelectColumn::ColumnAlias{p.at(0).get<std::string>(), p.at(1).get<std::string>()});
    else if (tag == "Agg") {
        std::optional<CondExpr> filter;
        if (p.contains("filter")) filter = p.at("filter").get<std::optional<CondExpr>>();
        col = SelectColumn(SelectColumn::Agg{p.at("func").get<AggFunc>(), p.at("col").get<std::string>(), filter});
    } else if (tag == "AggAlias") {
        std::optional<CondExpr> filter;
        if (p.contains("filter")) filter = p.at("filter").get<std::optional<CondExpr>>();
        col = SelectColumn(
            SelectColumn::AggAlias{p.at("func").get<AggFunc>(), p.at("col").get<std::string>(), p.at("alias").get<std::string>(), filter});
    }
    else if (tag == "Func")
        col = SelectColumn(SelectColumn::Func{p.at("name").get<std::string>(), p.at("args").get<std::vector<std::string>>(),
                                               p.at("alias").get<std::optional<std::string>>()});
    else if (tag == "Expr")
        col = SelectColumn(SelectColumn::Expr{p.at("expr").get<ArithExpr>(), p.at("alias").get<std::optional<std::string>>()});
    else if (tag == "CaseWhen")
        col = SelectColumn(SelectColumn::CaseWhen{p.at("branches").get<std::vector<CaseWhenBranch>>(),
                                                   p.at("else_val").get<std::optional<std::string>>(),
                                                   p.at("alias").get<std::optional<std::string>>()});
    else if (tag == "WinFunc") {
        SelectColumn::WinFunc wf;
        wf.func = p.at("func").get<WindowFunc>();
        wf.col = p.at("col").get<std::optional<std::string>>();
        wf.offset = p.at("offset").get<std::int64_t>();
        wf.partition_by = p.at("partition_by").get<std::vector<std::string>>();
        wf.order_by = p.at("order_by").get<std::vector<OrderBy>>();
        wf.alias = p.at("alias").get<std::optional<std::string>>();
        wf.frame = p.at("frame").get<std::optional<WindowFrame>>();
        col = SelectColumn(std::move(wf));
    } else if (tag == "Subquery") {
        col = SelectColumn(SelectColumn::Subquery{std::make_unique<Statement>(p.at("query").get<Statement>()),
                                                    p.at("alias").get<std::optional<std::string>>()});
    } else {
        throw std::runtime_error("unknown SelectColumn tag: " + tag);
    }
}

void to_json(nlohmann::json& j, const AlterAction& action) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AlterAction::AddColumn>) j = nlohmann::json{{"AddColumn", alt.column}};
            else if constexpr (std::is_same_v<T, AlterAction::DropColumn>) j = nlohmann::json{{"DropColumn", alt.name}};
            else if constexpr (std::is_same_v<T, AlterAction::RenameColumn>)
                j = nlohmann::json{{"RenameColumn", nlohmann::json{{"from", alt.from}, {"to", alt.to}}}};
            else if constexpr (std::is_same_v<T, AlterAction::ModifyColumn>) j = nlohmann::json{{"ModifyColumn", alt.column}};
            else if constexpr (std::is_same_v<T, AlterAction::RenameTable>) j = nlohmann::json{{"RenameTable", alt.to}};
            else if constexpr (std::is_same_v<T, AlterAction::AddForeignKey>)
                j = nlohmann::json{{"AddForeignKey", nlohmann::json{{"name", alt.name},
                                                                     {"column", alt.column},
                                                                     {"ref_table", alt.ref_table},
                                                                     {"ref_column", alt.ref_column},
                                                                     {"on_delete", alt.on_delete},
                                                                     {"on_update", alt.on_update}}}};
            else if constexpr (std::is_same_v<T, AlterAction::DropForeignKey>) j = nlohmann::json{{"DropForeignKey", alt.name}};
            else if constexpr (std::is_same_v<T, AlterAction::AddUniqueConstraint>)
                j = nlohmann::json{{"AddUniqueConstraint", nlohmann::json{{"name", alt.name}, {"column", alt.column}}}};
            else if constexpr (std::is_same_v<T, AlterAction::AddCheckConstraint>)
                j = nlohmann::json{{"AddCheckConstraint", nlohmann::json{{"name", alt.name}, {"expr", alt.expr}}}};
            else if constexpr (std::is_same_v<T, AlterAction::DropConstraint>) j = nlohmann::json{{"DropConstraint", alt.name}};
            else if constexpr (std::is_same_v<T, AlterAction::AddPartition>) j = nlohmann::json{{"AddPartition", alt.def}};
            else if constexpr (std::is_same_v<T, AlterAction::DropPartition>) j = nlohmann::json{{"DropPartition", alt.name}};
        },
        action.data);
}

void from_json(const nlohmann::json& j, AlterAction& action) {
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid AlterAction JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();
    if (tag == "AddColumn") action = AlterAction(AlterAction::AddColumn{p.get<ColumnDef>()});
    else if (tag == "DropColumn") action = AlterAction(AlterAction::DropColumn{p.get<std::string>()});
    else if (tag == "RenameColumn") action = AlterAction(AlterAction::RenameColumn{p.at("from").get<std::string>(), p.at("to").get<std::string>()});
    else if (tag == "ModifyColumn") action = AlterAction(AlterAction::ModifyColumn{p.get<ColumnDef>()});
    else if (tag == "RenameTable") action = AlterAction(AlterAction::RenameTable{p.get<std::string>()});
    else if (tag == "AddForeignKey")
        action = AlterAction(AlterAction::AddForeignKey{p.at("name").get<std::optional<std::string>>(), p.at("column").get<std::string>(),
                                                          p.at("ref_table").get<std::string>(), p.at("ref_column").get<std::string>(),
                                                          p.at("on_delete").get<FkAction>(), p.at("on_update").get<FkAction>()});
    else if (tag == "DropForeignKey") action = AlterAction(AlterAction::DropForeignKey{p.get<std::string>()});
    else if (tag == "AddUniqueConstraint")
        action = AlterAction(AlterAction::AddUniqueConstraint{p.at("name").get<std::optional<std::string>>(), p.at("column").get<std::string>()});
    else if (tag == "AddCheckConstraint")
        action = AlterAction(AlterAction::AddCheckConstraint{p.at("name").get<std::optional<std::string>>(), p.at("expr").get<std::string>()});
    else if (tag == "DropConstraint") action = AlterAction(AlterAction::DropConstraint{p.get<std::string>()});
    else if (tag == "AddPartition") action = AlterAction(AlterAction::AddPartition{p.get<PartitionDef>()});
    else if (tag == "DropPartition") action = AlterAction(AlterAction::DropPartition{p.get<std::string>()});
    else throw std::runtime_error("unknown AlterAction tag: " + tag);
}

void to_json(nlohmann::json& j, const Statement& stmt) {
    std::visit(
        [&j](const auto& alt) {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, Statement::Begin>) j = "Begin";
            else if constexpr (std::is_same_v<T, Statement::Commit>) j = "Commit";
            else if constexpr (std::is_same_v<T, Statement::Rollback>) j = "Rollback";
            else if constexpr (std::is_same_v<T, Statement::ShowTables>) j = "ShowTables";
            else if constexpr (std::is_same_v<T, Statement::ShowBufferPool>) j = "ShowBufferPool";
            else if constexpr (std::is_same_v<T, Statement::ShowWal>) j = "ShowWal";
            else if constexpr (std::is_same_v<T, Statement::Checkpoint>) j = "Checkpoint";
            else if constexpr (std::is_same_v<T, Statement::ShowIsolationLevel>) j = "ShowIsolationLevel";
            else if constexpr (std::is_same_v<T, Statement::ShowLocks>) j = "ShowLocks";
            else if constexpr (std::is_same_v<T, Statement::ShowRoles>) j = "ShowRoles";
            else if constexpr (std::is_same_v<T, Statement::ShowSynonyms>) j = "ShowSynonyms";
            else if constexpr (std::is_same_v<T, Statement::ShowDatabases>) j = "ShowDatabases";
            else if constexpr (std::is_same_v<T, Statement::ShowProcessList>) j = "ShowProcessList";
            else if constexpr (std::is_same_v<T, Statement::CreateTable>)
                j = nlohmann::json{{"CreateTable", nlohmann::json{{"name", alt.name},
                                                                   {"columns", alt.columns},
                                                                   {"if_not_exists", alt.if_not_exists},
                                                                   {"primary_key_columns", alt.primary_key_columns},
                                                                   {"check_constraints", alt.check_constraints},
                                                                   {"partition_by", alt.partition_by}}}};
            else if constexpr (std::is_same_v<T, Statement::DropTable>)
                j = nlohmann::json{{"DropTable", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::TruncateTable>)
                j = nlohmann::json{{"TruncateTable", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::Insert>)
                j = nlohmann::json{{"Insert", nlohmann::json{{"table", alt.table},
                                                              {"columns", alt.columns},
                                                              {"values", alt.values},
                                                              {"on_conflict", alt.on_conflict},
                                                              {"returning", alt.returning}}}};
            else if constexpr (std::is_same_v<T, Statement::InsertSelect>)
                j = nlohmann::json{{"InsertSelect", nlohmann::json{{"table", alt.table},
                                                                    {"columns", alt.columns},
                                                                    {"query", *alt.query},
                                                                    {"on_conflict", alt.on_conflict},
                                                                    {"returning", alt.returning}}}};
            else if constexpr (std::is_same_v<T, Statement::Select>) {
                nlohmann::json sub = nullptr;
                if (alt.subquery) sub = nlohmann::json::array({*alt.subquery->first, alt.subquery->second});
                j = nlohmann::json{{"Select", nlohmann::json{{"table", alt.table},
                                                              {"subquery", sub},
                                                              {"columns", alt.columns},
                                                              {"distinct", alt.distinct},
                                                              {"condition", alt.condition},
                                                              {"joins", alt.joins},
                                                              {"order_by", alt.order_by},
                                                              {"group_by", alt.group_by},
                                                              {"having", alt.having},
                                                              {"limit", alt.limit},
                                                              {"offset", alt.offset},
                                                              {"for_update", alt.for_update},
                                                              {"for_share", alt.for_share}}}};
            } else if constexpr (std::is_same_v<T, Statement::Update>)
                j = nlohmann::json{{"Update", nlohmann::json{{"table", alt.table},
                                                              {"assignments", alt.assignments},
                                                              {"condition", alt.condition},
                                                              {"returning", alt.returning}}}};
            else if constexpr (std::is_same_v<T, Statement::Delete>)
                j = nlohmann::json{{"Delete", nlohmann::json{{"table", alt.table}, {"condition", alt.condition}, {"returning", alt.returning}}}};
            else if constexpr (std::is_same_v<T, Statement::AlterTable>)
                j = nlohmann::json{{"AlterTable", nlohmann::json{{"table", alt.table}, {"action", alt.action}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateIndex>)
                j = nlohmann::json{{"CreateIndex", nlohmann::json{{"index_name", alt.index_name},
                                                                   {"table", alt.table},
                                                                   {"columns", alt.columns},
                                                                   {"using_hash", alt.using_hash}}}};
            else if constexpr (std::is_same_v<T, Statement::DropIndex>) j = nlohmann::json{{"DropIndex", nlohmann::json{{"index_name", alt.index_name}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateView>)
                j = nlohmann::json{{"CreateView", nlohmann::json{{"name", alt.name}, {"query", *alt.query}, {"raw_sql", alt.raw_sql}}}};
            else if constexpr (std::is_same_v<T, Statement::DropView>) j = nlohmann::json{{"DropView", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::Describe>) j = nlohmann::json{{"Describe", nlohmann::json{{"table", alt.table}}}};
            else if constexpr (std::is_same_v<T, Statement::SetIsolationLevel>)
                j = nlohmann::json{{"SetIsolationLevel", nlohmann::json{{"level", alt.level}}}};
            else if constexpr (std::is_same_v<T, Statement::Vacuum>) j = nlohmann::json{{"Vacuum", nlohmann::json{{"table", alt.table}}}};
            else if constexpr (std::is_same_v<T, Statement::Use>) j = nlohmann::json{{"Use", nlohmann::json{{"database", alt.database}}}};
            else if constexpr (std::is_same_v<T, Statement::Savepoint>) j = nlohmann::json{{"Savepoint", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::ReleaseSavepoint>)
                j = nlohmann::json{{"ReleaseSavepoint", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::RollbackTo>) j = nlohmann::json{{"RollbackTo", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::Explain>) j = nlohmann::json{{"Explain", *alt.inner}};
            else if constexpr (std::is_same_v<T, Statement::ExplainAnalyze>) j = nlohmann::json{{"ExplainAnalyze", *alt.inner}};
            else if constexpr (std::is_same_v<T, Statement::AnalyzeTable>) j = nlohmann::json{{"AnalyzeTable", nlohmann::json{{"table", alt.table}}}};
            else if constexpr (std::is_same_v<T, Statement::With>) {
                nlohmann::json ctes = nlohmann::json::array();
                for (auto& item : alt.ctes) ctes.push_back(nlohmann::json::array({item.first, *item.second}));
                j = nlohmann::json{{"With", nlohmann::json{{"ctes", ctes}, {"query", *alt.query}, {"recursive", alt.recursive}}}};
            } else if constexpr (std::is_same_v<T, Statement::Union>)
                j = nlohmann::json{{"Union", nlohmann::json{{"left", *alt.left},
                                                             {"right", *alt.right},
                                                             {"all", alt.all},
                                                             {"order_by", alt.order_by},
                                                             {"limit", alt.limit},
                                                             {"offset", alt.offset}}}};
            else if constexpr (std::is_same_v<T, Statement::Intersect>)
                j = nlohmann::json{{"Intersect", nlohmann::json{{"left", *alt.left},
                                                                 {"right", *alt.right},
                                                                 {"all", alt.all},
                                                                 {"order_by", alt.order_by},
                                                                 {"limit", alt.limit},
                                                                 {"offset", alt.offset}}}};
            else if constexpr (std::is_same_v<T, Statement::Except>)
                j = nlohmann::json{{"Except", nlohmann::json{{"left", *alt.left},
                                                              {"right", *alt.right},
                                                              {"all", alt.all},
                                                              {"order_by", alt.order_by},
                                                              {"limit", alt.limit},
                                                              {"offset", alt.offset}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateDatabase>)
                j = nlohmann::json{{"CreateDatabase", nlohmann::json{{"name", alt.name}, {"if_not_exists", alt.if_not_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::DropDatabase>)
                j = nlohmann::json{{"DropDatabase", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::MultiUpdate>)
                j = nlohmann::json{{"MultiUpdate", nlohmann::json{{"tables", alt.tables},
                                                                   {"joins", alt.joins},
                                                                   {"assignments", alt.assignments},
                                                                   {"condition", alt.condition}}}};
            else if constexpr (std::is_same_v<T, Statement::MultiDelete>)
                j = nlohmann::json{{"MultiDelete", nlohmann::json{{"delete_tables", alt.delete_tables},
                                                                   {"from_table", alt.from_table},
                                                                   {"joins", alt.joins},
                                                                   {"condition", alt.condition}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateUser>)
                j = nlohmann::json{{"CreateUser", nlohmann::json{{"user", alt.user},
                                                                  {"host", alt.host},
                                                                  {"password", alt.password},
                                                                  {"if_not_exists", alt.if_not_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::DropUser>)
                j = nlohmann::json{{"DropUser", nlohmann::json{{"user", alt.user}, {"host", alt.host}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::Grant>)
                j = nlohmann::json{{"Grant", nlohmann::json{{"privileges", alt.privileges},
                                                             {"object_type", alt.object_type},
                                                             {"object", alt.object},
                                                             {"user", alt.user},
                                                             {"host", alt.host},
                                                             {"with_grant_option", alt.with_grant_option}}}};
            else if constexpr (std::is_same_v<T, Statement::Revoke>)
                j = nlohmann::json{{"Revoke", nlohmann::json{{"privileges", alt.privileges},
                                                              {"object_type", alt.object_type},
                                                              {"object", alt.object},
                                                              {"user", alt.user},
                                                              {"host", alt.host}}}};
            else if constexpr (std::is_same_v<T, Statement::ShowGrants>)
                j = nlohmann::json{{"ShowGrants", nlohmann::json{{"user", alt.user}, {"host", alt.host}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateRole>) j = nlohmann::json{{"CreateRole", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::DropRole>)
                j = nlohmann::json{{"DropRole", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::GrantRole>)
                j = nlohmann::json{{"GrantRole", nlohmann::json{{"role", alt.role},
                                                                 {"user", alt.user},
                                                                 {"host", alt.host},
                                                                 {"with_admin_option", alt.with_admin_option}}}};
            else if constexpr (std::is_same_v<T, Statement::RevokeRole>)
                j = nlohmann::json{{"RevokeRole", nlohmann::json{{"role", alt.role}, {"user", alt.user}, {"host", alt.host}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateSynonym>)
                j = nlohmann::json{{"CreateSynonym", nlohmann::json{{"name", alt.name}, {"target", alt.target}, {"or_replace", alt.or_replace}}}};
            else if constexpr (std::is_same_v<T, Statement::DropSynonym>)
                j = nlohmann::json{{"DropSynonym", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::ShowCreateTable>)
                j = nlohmann::json{{"ShowCreateTable", nlohmann::json{{"table", alt.table}}}};
            else if constexpr (std::is_same_v<T, Statement::ShowCreateView>)
                j = nlohmann::json{{"ShowCreateView", nlohmann::json{{"view", alt.view}}}};
            else if constexpr (std::is_same_v<T, Statement::ShowIndex>) j = nlohmann::json{{"ShowIndex", nlohmann::json{{"table", alt.table}}}};
            else if constexpr (std::is_same_v<T, Statement::Merge>)
                j = nlohmann::json{{"Merge", nlohmann::json{{"target", alt.target},
                                                             {"target_alias", alt.target_alias},
                                                             {"source", alt.source},
                                                             {"source_alias", alt.source_alias},
                                                             {"on", alt.on},
                                                             {"when_matched_update", alt.when_matched_update},
                                                             {"when_matched_delete", alt.when_matched_delete},
                                                             {"when_matched_delete_cond", alt.when_matched_delete_cond},
                                                             {"when_not_matched_columns", alt.when_not_matched_columns},
                                                             {"when_not_matched_values", alt.when_not_matched_values}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateProcedure>)
                j = nlohmann::json{{"CreateProcedure", nlohmann::json{{"name", alt.name}, {"params", alt.params}, {"body", alt.body}}}};
            else if constexpr (std::is_same_v<T, Statement::CallProcedure>)
                j = nlohmann::json{{"CallProcedure", nlohmann::json{{"name", alt.name}, {"args", alt.args}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateTrigger>)
                j = nlohmann::json{{"CreateTrigger", nlohmann::json{{"name", alt.name},
                                                                     {"timing", alt.timing},
                                                                     {"event", alt.event},
                                                                     {"table", alt.table},
                                                                     {"body", alt.body}}}};
            else if constexpr (std::is_same_v<T, Statement::DropTrigger>)
                j = nlohmann::json{{"DropTrigger", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::DropProcedure>)
                j = nlohmann::json{{"DropProcedure", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::Backup>)
                j = nlohmann::json{{"Backup", nlohmann::json{{"database", alt.database}, {"output_file", alt.output_file}}}};
            else if constexpr (std::is_same_v<T, Statement::Restore>)
                j = nlohmann::json{{"Restore", nlohmann::json{{"source_file", alt.source_file}, {"database", alt.database}}}};
            else if constexpr (std::is_same_v<T, Statement::CreateFunction>)
                j = nlohmann::json{{"CreateFunction", nlohmann::json{{"name", alt.name}, {"params", alt.params}, {"body", alt.body}}}};
            else if constexpr (std::is_same_v<T, Statement::DropFunction>)
                j = nlohmann::json{{"DropFunction", nlohmann::json{{"name", alt.name}, {"if_exists", alt.if_exists}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcDeclare>)
                j = nlohmann::json{{"ProcDeclare", nlohmann::json{{"name", alt.name}, {"typ", alt.typ}, {"default_value", alt.default_value}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcSet>)
                j = nlohmann::json{{"ProcSet", nlohmann::json{{"name", alt.name}, {"expr", alt.expr}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcIf>)
                j = nlohmann::json{{"ProcIf", nlohmann::json{{"condition", alt.condition},
                                                              {"then_body", alt.then_body},
                                                              {"elseif_branches", alt.elseif_branches},
                                                              {"else_body", alt.else_body}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcWhile>)
                j = nlohmann::json{{"ProcWhile", nlohmann::json{{"label", alt.label}, {"condition", alt.condition}, {"body", alt.body}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcLoop>)
                j = nlohmann::json{{"ProcLoop", nlohmann::json{{"label", alt.label}, {"body", alt.body}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcRepeat>)
                j = nlohmann::json{{"ProcRepeat", nlohmann::json{{"label", alt.label}, {"body", alt.body}, {"until", alt.until}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcLeave>) j = nlohmann::json{{"ProcLeave", nlohmann::json{{"label", alt.label}}}};
            else if constexpr (std::is_same_v<T, Statement::ProcIterate>) j = nlohmann::json{{"ProcIterate", nlohmann::json{{"label", alt.label}}}};
            else if constexpr (std::is_same_v<T, Statement::PrepareStmt>)
                j = nlohmann::json{{"PrepareStmt", nlohmann::json{{"name", alt.name}, {"query", alt.query}}}};
            else if constexpr (std::is_same_v<T, Statement::ExecuteStmt>)
                j = nlohmann::json{{"ExecuteStmt", nlohmann::json{{"name", alt.name}, {"using_vars", alt.using_vars}}}};
            else if constexpr (std::is_same_v<T, Statement::DeallocatePrepare>)
                j = nlohmann::json{{"DeallocatePrepare", nlohmann::json{{"name", alt.name}}}};
            else if constexpr (std::is_same_v<T, Statement::SetUserVar>)
                j = nlohmann::json{{"SetUserVar", nlohmann::json{{"name", alt.name}, {"expr", alt.expr}}}};
        },
        stmt.data);
}

void from_json(const nlohmann::json& j, Statement& stmt) {
    if (j.is_string()) {
        const std::string tag = j.get<std::string>();
        if (tag == "Begin") stmt = Statement::Begin{};
        else if (tag == "Commit") stmt = Statement::Commit{};
        else if (tag == "Rollback") stmt = Statement::Rollback{};
        else if (tag == "ShowTables") stmt = Statement::ShowTables{};
        else if (tag == "ShowBufferPool") stmt = Statement::ShowBufferPool{};
        else if (tag == "ShowWal") stmt = Statement::ShowWal{};
        else if (tag == "Checkpoint") stmt = Statement::Checkpoint{};
        else if (tag == "ShowIsolationLevel") stmt = Statement::ShowIsolationLevel{};
        else if (tag == "ShowLocks") stmt = Statement::ShowLocks{};
        else if (tag == "ShowRoles") stmt = Statement::ShowRoles{};
        else if (tag == "ShowSynonyms") stmt = Statement::ShowSynonyms{};
        else if (tag == "ShowDatabases") stmt = Statement::ShowDatabases{};
        else if (tag == "ShowProcessList") stmt = Statement::ShowProcessList{};
        else throw std::runtime_error("unknown Statement tag: " + tag);
        return;
    }
    if (!j.is_object() || j.empty()) throw std::runtime_error("invalid Statement JSON");
    auto it = j.begin();
    const std::string& tag = it.key();
    const auto& p = it.value();

    if (tag == "CreateTable") {
        Statement::CreateTable v;
        v.name = p.at("name").get<std::string>();
        v.columns = p.at("columns").get<std::vector<ColumnDef>>();
        v.if_not_exists = p.at("if_not_exists").get<bool>();
        v.primary_key_columns = p.at("primary_key_columns").get<std::vector<std::string>>();
        v.check_constraints = p.at("check_constraints").get<std::vector<std::pair<std::optional<std::string>, std::string>>>();
        if (p.contains("partition_by")) v.partition_by = p.at("partition_by").get<std::optional<PartitionBy>>();
        stmt = Statement(std::move(v));
    } else if (tag == "DropTable") {
        stmt = Statement(Statement::DropTable{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "TruncateTable") {
        stmt = Statement(Statement::TruncateTable{p.at("name").get<std::string>()});
    } else if (tag == "Insert") {
        Statement::Insert v;
        v.table = p.at("table").get<std::string>();
        v.columns = p.at("columns").get<std::optional<std::vector<std::string>>>();
        v.values = p.at("values").get<std::vector<std::vector<std::string>>>();
        v.on_conflict = p.at("on_conflict").get<InsertConflict>();
        v.returning = p.at("returning").get<std::optional<std::vector<SelectColumn>>>();
        stmt = Statement(std::move(v));
    } else if (tag == "InsertSelect") {
        Statement::InsertSelect v;
        v.table = p.at("table").get<std::string>();
        v.columns = p.at("columns").get<std::optional<std::vector<std::string>>>();
        v.query = std::make_unique<Statement>(p.at("query").get<Statement>());
        v.on_conflict = p.at("on_conflict").get<InsertConflict>();
        v.returning = p.at("returning").get<std::optional<std::vector<SelectColumn>>>();
        stmt = Statement(std::move(v));
    } else if (tag == "Select") {
        Statement::Select v;
        v.table = p.at("table").get<std::string>();
        if (!p.at("subquery").is_null()) {
            auto sub_j = p.at("subquery");
            v.subquery = std::make_pair(std::make_unique<Statement>(sub_j.at(0).get<Statement>()), sub_j.at(1).get<std::string>());
        }
        v.columns = p.at("columns").get<std::vector<SelectColumn>>();
        v.distinct = p.at("distinct").get<bool>();
        v.condition = p.at("condition").get<std::optional<CondExpr>>();
        v.joins = p.at("joins").get<std::vector<Join>>();
        v.order_by = p.at("order_by").get<std::vector<OrderBy>>();
        v.group_by = p.at("group_by").get<std::optional<std::vector<std::string>>>();
        v.having = p.at("having").get<std::optional<CondExpr>>();
        v.limit = p.at("limit").get<std::optional<std::size_t>>();
        v.offset = p.at("offset").get<std::optional<std::size_t>>();
        v.for_update = p.at("for_update").get<bool>();
        v.for_share = p.at("for_share").get<bool>();
        stmt = Statement(std::move(v));
    } else if (tag == "Update") {
        Statement::Update v;
        v.table = p.at("table").get<std::string>();
        v.assignments = p.at("assignments").get<std::vector<std::pair<std::string, ArithExpr>>>();
        v.condition = p.at("condition").get<std::optional<CondExpr>>();
        v.returning = p.at("returning").get<std::optional<std::vector<SelectColumn>>>();
        stmt = Statement(std::move(v));
    } else if (tag == "Delete") {
        Statement::Delete v;
        v.table = p.at("table").get<std::string>();
        v.condition = p.at("condition").get<std::optional<CondExpr>>();
        v.returning = p.at("returning").get<std::optional<std::vector<SelectColumn>>>();
        stmt = Statement(std::move(v));
    } else if (tag == "AlterTable") {
        stmt = Statement(Statement::AlterTable{p.at("table").get<std::string>(), p.at("action").get<AlterAction>()});
    } else if (tag == "CreateIndex") {
        stmt = Statement(Statement::CreateIndex{p.at("index_name").get<std::string>(), p.at("table").get<std::string>(),
                                                  p.at("columns").get<std::vector<std::string>>(), p.at("using_hash").get<bool>()});
    } else if (tag == "DropIndex") {
        stmt = Statement(Statement::DropIndex{p.at("index_name").get<std::string>()});
    } else if (tag == "CreateView") {
        stmt = Statement(Statement::CreateView{p.at("name").get<std::string>(), std::make_unique<Statement>(p.at("query").get<Statement>()),
                                                p.at("raw_sql").get<std::string>()});
    } else if (tag == "DropView") {
        stmt = Statement(Statement::DropView{p.at("name").get<std::string>()});
    } else if (tag == "Describe") {
        stmt = Statement(Statement::Describe{p.at("table").get<std::string>()});
    } else if (tag == "SetIsolationLevel") {
        stmt = Statement(Statement::SetIsolationLevel{p.at("level").get<IsolationLevel>()});
    } else if (tag == "Vacuum") {
        stmt = Statement(Statement::Vacuum{p.at("table").get<std::optional<std::string>>()});
    } else if (tag == "Use") {
        stmt = Statement(Statement::Use{p.at("database").get<std::string>()});
    } else if (tag == "Savepoint") {
        stmt = Statement(Statement::Savepoint{p.at("name").get<std::string>()});
    } else if (tag == "ReleaseSavepoint") {
        stmt = Statement(Statement::ReleaseSavepoint{p.at("name").get<std::string>()});
    } else if (tag == "RollbackTo") {
        stmt = Statement(Statement::RollbackTo{p.at("name").get<std::string>()});
    } else if (tag == "Explain") {
        stmt = Statement(Statement::Explain{std::make_unique<Statement>(p.get<Statement>())});
    } else if (tag == "ExplainAnalyze") {
        stmt = Statement(Statement::ExplainAnalyze{std::make_unique<Statement>(p.get<Statement>())});
    } else if (tag == "AnalyzeTable") {
        stmt = Statement(Statement::AnalyzeTable{p.at("table").get<std::string>()});
    } else if (tag == "With") {
        Statement::With v;
        for (auto& item : p.at("ctes")) {
            v.ctes.emplace_back(item.at(0).get<std::string>(), std::make_unique<Statement>(item.at(1).get<Statement>()));
        }
        v.query = std::make_unique<Statement>(p.at("query").get<Statement>());
        v.recursive = p.at("recursive").get<bool>();
        stmt = Statement(std::move(v));
    } else if (tag == "Union" || tag == "Intersect" || tag == "Except") {
        auto left = std::make_unique<Statement>(p.at("left").get<Statement>());
        auto right = std::make_unique<Statement>(p.at("right").get<Statement>());
        bool all = p.at("all").get<bool>();
        auto order_by = p.at("order_by").get<std::vector<OrderBy>>();
        auto limit = p.at("limit").get<std::optional<std::size_t>>();
        auto offset = p.at("offset").get<std::optional<std::size_t>>();
        if (tag == "Union") stmt = Statement(Statement::Union{std::move(left), std::move(right), all, order_by, limit, offset});
        else if (tag == "Intersect") stmt = Statement(Statement::Intersect{std::move(left), std::move(right), all, order_by, limit, offset});
        else stmt = Statement(Statement::Except{std::move(left), std::move(right), all, order_by, limit, offset});
    } else if (tag == "CreateDatabase") {
        stmt = Statement(Statement::CreateDatabase{p.at("name").get<std::string>(), p.at("if_not_exists").get<bool>()});
    } else if (tag == "DropDatabase") {
        stmt = Statement(Statement::DropDatabase{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "MultiUpdate") {
        Statement::MultiUpdate v;
        v.tables = p.at("tables").get<std::vector<std::string>>();
        v.joins = p.at("joins").get<std::vector<Join>>();
        v.assignments = p.at("assignments").get<std::vector<std::pair<std::string, ArithExpr>>>();
        v.condition = p.at("condition").get<std::optional<CondExpr>>();
        stmt = Statement(std::move(v));
    } else if (tag == "MultiDelete") {
        Statement::MultiDelete v;
        v.delete_tables = p.at("delete_tables").get<std::vector<std::string>>();
        v.from_table = p.at("from_table").get<std::string>();
        v.joins = p.at("joins").get<std::vector<Join>>();
        v.condition = p.at("condition").get<std::optional<CondExpr>>();
        stmt = Statement(std::move(v));
    } else if (tag == "CreateUser") {
        stmt = Statement(Statement::CreateUser{p.at("user").get<std::string>(), p.at("host").get<std::string>(),
                                                 p.at("password").get<std::optional<std::string>>(), p.at("if_not_exists").get<bool>()});
    } else if (tag == "DropUser") {
        stmt = Statement(Statement::DropUser{p.at("user").get<std::string>(), p.at("host").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "Grant") {
        stmt = Statement(Statement::Grant{p.at("privileges").get<std::vector<std::string>>(), p.at("object_type").get<std::string>(),
                                           p.at("object").get<std::string>(), p.at("user").get<std::string>(), p.at("host").get<std::string>(),
                                           p.at("with_grant_option").get<bool>()});
    } else if (tag == "Revoke") {
        stmt = Statement(Statement::Revoke{p.at("privileges").get<std::vector<std::string>>(), p.at("object_type").get<std::string>(),
                                            p.at("object").get<std::string>(), p.at("user").get<std::string>(), p.at("host").get<std::string>()});
    } else if (tag == "ShowGrants") {
        stmt = Statement(Statement::ShowGrants{p.at("user").get<std::optional<std::string>>(), p.at("host").get<std::optional<std::string>>()});
    } else if (tag == "CreateRole") {
        stmt = Statement(Statement::CreateRole{p.at("name").get<std::string>()});
    } else if (tag == "DropRole") {
        stmt = Statement(Statement::DropRole{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "GrantRole") {
        stmt = Statement(Statement::GrantRole{p.at("role").get<std::string>(), p.at("user").get<std::string>(), p.at("host").get<std::string>(),
                                               p.at("with_admin_option").get<bool>()});
    } else if (tag == "RevokeRole") {
        stmt = Statement(Statement::RevokeRole{p.at("role").get<std::string>(), p.at("user").get<std::string>(), p.at("host").get<std::string>()});
    } else if (tag == "CreateSynonym") {
        stmt = Statement(
            Statement::CreateSynonym{p.at("name").get<std::string>(), p.at("target").get<std::string>(), p.at("or_replace").get<bool>()});
    } else if (tag == "DropSynonym") {
        stmt = Statement(Statement::DropSynonym{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "ShowCreateTable") {
        stmt = Statement(Statement::ShowCreateTable{p.at("table").get<std::string>()});
    } else if (tag == "ShowCreateView") {
        stmt = Statement(Statement::ShowCreateView{p.at("view").get<std::string>()});
    } else if (tag == "ShowIndex") {
        stmt = Statement(Statement::ShowIndex{p.at("table").get<std::string>()});
    } else if (tag == "Merge") {
        Statement::Merge v;
        v.target = p.at("target").get<std::string>();
        v.target_alias = p.at("target_alias").get<std::optional<std::string>>();
        v.source = p.at("source").get<std::string>();
        v.source_alias = p.at("source_alias").get<std::optional<std::string>>();
        v.on = p.at("on").get<CondExpr>();
        v.when_matched_update = p.at("when_matched_update").get<std::optional<std::vector<std::pair<std::string, ArithExpr>>>>();
        v.when_matched_delete = p.at("when_matched_delete").get<bool>();
        v.when_matched_delete_cond = p.at("when_matched_delete_cond").get<std::optional<CondExpr>>();
        v.when_not_matched_columns = p.at("when_not_matched_columns").get<std::optional<std::vector<std::string>>>();
        v.when_not_matched_values = p.at("when_not_matched_values").get<std::vector<std::string>>();
        stmt = Statement(std::move(v));
    } else if (tag == "CreateProcedure") {
        Statement::CreateProcedure v;
        v.name = p.at("name").get<std::string>();
        v.params = p.at("params").get<std::vector<std::tuple<std::string, std::string, std::string>>>();
        v.body = p.at("body").get<std::vector<Statement>>();
        stmt = Statement(std::move(v));
    } else if (tag == "CallProcedure") {
        stmt = Statement(Statement::CallProcedure{p.at("name").get<std::string>(), p.at("args").get<std::vector<std::string>>()});
    } else if (tag == "CreateTrigger") {
        Statement::CreateTrigger v;
        v.name = p.at("name").get<std::string>();
        v.timing = p.at("timing").get<TriggerTiming>();
        v.event = p.at("event").get<TriggerEvent>();
        v.table = p.at("table").get<std::string>();
        v.body = p.at("body").get<std::vector<Statement>>();
        stmt = Statement(std::move(v));
    } else if (tag == "DropTrigger") {
        stmt = Statement(Statement::DropTrigger{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "DropProcedure") {
        stmt = Statement(Statement::DropProcedure{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "Backup") {
        stmt = Statement(Statement::Backup{p.at("database").get<std::optional<std::string>>(), p.at("output_file").get<std::optional<std::string>>()});
    } else if (tag == "Restore") {
        stmt = Statement(Statement::Restore{p.at("source_file").get<std::string>(), p.at("database").get<std::optional<std::string>>()});
    } else if (tag == "CreateFunction") {
        stmt = Statement(Statement::CreateFunction{p.at("name").get<std::string>(), p.at("params").get<std::vector<std::string>>(),
                                                     p.at("body").get<std::string>()});
    } else if (tag == "DropFunction") {
        stmt = Statement(Statement::DropFunction{p.at("name").get<std::string>(), p.at("if_exists").get<bool>()});
    } else if (tag == "ProcDeclare") {
        stmt = Statement(Statement::ProcDeclare{p.at("name").get<std::string>(), p.at("typ").get<std::string>(),
                                                  p.at("default_value").get<std::optional<std::string>>()});
    } else if (tag == "ProcSet") {
        stmt = Statement(Statement::ProcSet{p.at("name").get<std::string>(), p.at("expr").get<ArithExpr>()});
    } else if (tag == "ProcIf") {
        Statement::ProcIf v;
        v.condition = p.at("condition").get<CondExpr>();
        v.then_body = p.at("then_body").get<std::vector<Statement>>();
        v.elseif_branches = p.at("elseif_branches").get<std::vector<std::pair<CondExpr, std::vector<Statement>>>>();
        v.else_body = p.at("else_body").get<std::optional<std::vector<Statement>>>();
        stmt = Statement(std::move(v));
    } else if (tag == "ProcWhile") {
        Statement::ProcWhile v;
        v.label = p.at("label").get<std::optional<std::string>>();
        v.condition = p.at("condition").get<CondExpr>();
        v.body = p.at("body").get<std::vector<Statement>>();
        stmt = Statement(std::move(v));
    } else if (tag == "ProcLoop") {
        Statement::ProcLoop v;
        v.label = p.at("label").get<std::optional<std::string>>();
        v.body = p.at("body").get<std::vector<Statement>>();
        stmt = Statement(std::move(v));
    } else if (tag == "ProcRepeat") {
        Statement::ProcRepeat v;
        v.label = p.at("label").get<std::optional<std::string>>();
        v.body = p.at("body").get<std::vector<Statement>>();
        v.until = p.at("until").get<CondExpr>();
        stmt = Statement(std::move(v));
    } else if (tag == "ProcLeave") {
        stmt = Statement(Statement::ProcLeave{p.at("label").get<std::optional<std::string>>()});
    } else if (tag == "ProcIterate") {
        stmt = Statement(Statement::ProcIterate{p.at("label").get<std::optional<std::string>>()});
    } else if (tag == "PrepareStmt") {
        stmt = Statement(Statement::PrepareStmt{p.at("name").get<std::string>(), p.at("query").get<std::string>()});
    } else if (tag == "ExecuteStmt") {
        stmt = Statement(Statement::ExecuteStmt{p.at("name").get<std::string>(), p.at("using_vars").get<std::vector<std::string>>()});
    } else if (tag == "DeallocatePrepare") {
        stmt = Statement(Statement::DeallocatePrepare{p.at("name").get<std::string>()});
    } else if (tag == "SetUserVar") {
        stmt = Statement(Statement::SetUserVar{p.at("name").get<std::string>(), p.at("expr").get<ArithExpr>()});
    } else {
        throw std::runtime_error("unknown Statement tag: " + tag);
    }
}

} // namespace engine
