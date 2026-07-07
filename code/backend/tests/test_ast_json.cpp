#include "catch.hpp"
#include "engine/parser/ast_json.hpp"

using namespace engine;

namespace {
// Round-trips a Statement through to_json/dump/parse/from_json, mirroring how
// DiskManager persists views/procedures/triggers as JSON text on disk.
Statement roundtrip(const Statement& s) {
    nlohmann::json j = s;
    std::string text = j.dump();
    return nlohmann::json::parse(text).get<Statement>();
}
} // namespace

TEST_CASE("Statement::CreateTable with FK and check constraints round-trips", "[ast_json]") {
    ColumnDef id_col;
    id_col.name = "id";
    id_col.primary_key = true;
    id_col.data_type = DataType(DataType::Int{});

    ColumnDef dept_col;
    dept_col.name = "dept_id";
    dept_col.data_type = DataType(DataType::Int{});
    ForeignKey fk;
    fk.column = "dept_id";
    fk.ref_table = "rusql.department";
    fk.ref_column = "id";
    fk.on_delete = FkAction::Cascade;
    dept_col.foreign_key = fk;

    Statement s = Statement::CreateTable{
        "rusql.employee", {id_col, dept_col}, true, {"id"}, {{std::optional<std::string>("chk_dept"), "dept_id > 0"}}};

    Statement back = roundtrip(s);
    auto& orig = std::get<Statement::CreateTable>(s.data);
    auto& ct = std::get<Statement::CreateTable>(back.data);
    REQUIRE(ct.name == orig.name);
    REQUIRE(ct.if_not_exists == orig.if_not_exists);
    REQUIRE(ct.primary_key_columns == orig.primary_key_columns);
    REQUIRE(ct.columns.size() == 2);
    REQUIRE(ct.columns[1].foreign_key.has_value());
    REQUIRE(ct.columns[1].foreign_key->ref_table == "rusql.department");
    REQUIRE(ct.columns[1].foreign_key->on_delete == FkAction::Cascade);
    REQUIRE(ct.check_constraints.size() == 1);
    REQUIRE(ct.check_constraints[0].first == std::optional<std::string>("chk_dept"));
    REQUIRE(ct.check_constraints[0].second == "dept_id > 0");
}

TEST_CASE("Statement::Select with joins/condition/order_by/group_by round-trips", "[ast_json]") {
    CondExpr cond = CondExpr(CondExpr::And{
        std::make_unique<CondExpr>(CondExpr::Leaf{
            Condition{ArithExpr(ArithExpr::Col{"age"}), Operator::Gte, ConditionValue(ConditionValue::Literal{"18"})}}),
        std::make_unique<CondExpr>(CondExpr::Leaf{
            Condition{ArithExpr(ArithExpr::Col{"dept.name"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"Eng"})}})});

    Join j;
    j.table = "rusql.department";
    j.join_type = JoinType::Left;
    j.on_expr = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"employee.dept_id"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"department.id"})}});

    Statement::Select sel;
    sel.table = "rusql.employee";
    sel.columns = {SelectColumn(SelectColumn::All{}), SelectColumn(SelectColumn::ColumnAlias{"name", "emp_name"})};
    sel.distinct = true;
    sel.condition = cond;
    sel.joins = {j};
    sel.order_by = {OrderBy{"age", false}};
    sel.group_by = std::vector<std::string>{"dept_id"};
    sel.having = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"cnt"}), Operator::Gt, ConditionValue(ConditionValue::Literal{"1"})}});
    sel.limit = 10;
    sel.offset = 5;
    sel.for_update = true;
    sel.for_share = false;
    Statement s(std::move(sel));

    Statement back = roundtrip(s);
    auto& r = std::get<Statement::Select>(back.data);
    REQUIRE(r.table == "rusql.employee");
    REQUIRE(r.columns.size() == 2);
    REQUIRE(std::holds_alternative<SelectColumn::ColumnAlias>(r.columns[1].data));
    REQUIRE(r.distinct);
    REQUIRE(r.condition.has_value());
    REQUIRE(std::holds_alternative<CondExpr::And>(r.condition->data));
    REQUIRE(r.joins.size() == 1);
    REQUIRE(r.joins[0].table == "rusql.department");
    REQUIRE(r.joins[0].join_type == JoinType::Left);
    REQUIRE(r.order_by.size() == 1);
    REQUIRE_FALSE(r.order_by[0].ascending);
    REQUIRE(r.group_by.has_value());
    REQUIRE((*r.group_by)[0] == "dept_id");
    REQUIRE(r.having.has_value());
    REQUIRE(r.limit == std::optional<std::size_t>(10));
    REQUIRE(r.offset == std::optional<std::size_t>(5));
    REQUIRE(r.for_update);
    REQUIRE_FALSE(r.for_share);
}

TEST_CASE("Statement::Select with a boxed subquery round-trips", "[ast_json]") {
    Statement inner = Statement::Select{
        "rusql.t", std::nullopt, {SelectColumn(SelectColumn::All{})}, false, std::nullopt, {}, {}, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, false, false};

    Statement::Select outer_sel;
    outer_sel.table = "";
    outer_sel.subquery = std::make_pair(std::make_unique<Statement>(inner), std::string("sub_alias"));
    outer_sel.columns = {SelectColumn(SelectColumn::All{})};
    Statement s(std::move(outer_sel));

    Statement back = roundtrip(s);
    auto& r = std::get<Statement::Select>(back.data);
    REQUIRE(r.subquery.has_value());
    REQUIRE(r.subquery->second == "sub_alias");
    auto& inner_back = std::get<Statement::Select>(r.subquery->first->data);
    REQUIRE(inner_back.table == "rusql.t");
}

TEST_CASE("ConditionValue::Subquery (IN subquery) round-trips", "[ast_json]") {
    Statement sub = Statement::Select{
        "rusql.department", std::nullopt, {SelectColumn(SelectColumn::Column{"id"})}, false, std::nullopt, {}, {},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};

    Condition cond{ArithExpr(ArithExpr::Col{"dept_id"}), Operator::In, ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(sub)})};
    Statement s = Statement::Select{
        "rusql.employee", std::nullopt, {SelectColumn(SelectColumn::All{})}, false,
        CondExpr(CondExpr::Leaf{cond}), {}, {}, std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};

    Statement back = roundtrip(s);
    auto& r = std::get<Statement::Select>(back.data);
    auto& leaf = std::get<CondExpr::Leaf>(r.condition->data);
    REQUIRE(leaf.condition.op == Operator::In);
    auto& subq = std::get<ConditionValue::Subquery>(leaf.condition.value.data);
    auto& subq_sel = std::get<Statement::Select>(subq.query->data);
    REQUIRE(subq_sel.table == "rusql.department");
}

TEST_CASE("Statement::With (recursive CTE over Union) round-trips", "[ast_json]") {
    Statement base = Statement::Select{
        "rusql.emp", std::nullopt, {SelectColumn(SelectColumn::Column{"id"})}, false, std::nullopt, {}, {},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};
    Statement rec = Statement::Select{
        "rusql.emp", std::nullopt, {SelectColumn(SelectColumn::Column{"id"})}, false, std::nullopt, {}, {},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};
    Statement uni = Statement::Union{std::make_unique<Statement>(base), std::make_unique<Statement>(rec), true, {}, std::nullopt, std::nullopt};

    Statement main_query = Statement::Select{
        "cte_result", std::nullopt, {SelectColumn(SelectColumn::All{})}, false, std::nullopt, {}, {},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};

    Statement::With w;
    w.ctes.emplace_back("cte_result", std::make_unique<Statement>(uni));
    w.query = std::make_unique<Statement>(main_query);
    w.recursive = true;
    Statement s(std::move(w));

    Statement back = roundtrip(s);
    auto& r = std::get<Statement::With>(back.data);
    REQUIRE(r.recursive);
    REQUIRE(r.ctes.size() == 1);
    REQUIRE(r.ctes[0].first == "cte_result");
    REQUIRE(std::holds_alternative<Statement::Union>(r.ctes[0].second->data));
    auto& u = std::get<Statement::Union>(r.ctes[0].second->data);
    REQUIRE(u.all);
    auto& query_back = std::get<Statement::Select>(r.query->data);
    REQUIRE(query_back.table == "cte_result");
}

TEST_CASE("Statement::Merge round-trips", "[ast_json]") {
    Statement::Merge m;
    m.target = "rusql.t1";
    m.target_alias = "t";
    m.source = "rusql.t2";
    m.source_alias = std::nullopt;
    m.on = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"t.id"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"s.id"})}});
    m.when_matched_update = std::vector<std::pair<std::string, ArithExpr>>{{"val", ArithExpr(ArithExpr::Num{"1"})}};
    m.when_matched_delete = false;
    m.when_not_matched_columns = std::vector<std::string>{"id", "val"};
    m.when_not_matched_values = {"1", "2"};
    Statement s(std::move(m));

    Statement back = roundtrip(s);
    auto& r = std::get<Statement::Merge>(back.data);
    REQUIRE(r.target == "rusql.t1");
    REQUIRE(r.target_alias == std::optional<std::string>("t"));
    REQUIRE_FALSE(r.source_alias.has_value());
    REQUIRE(r.when_matched_update.has_value());
    REQUIRE((*r.when_matched_update)[0].first == "val");
    REQUIRE_FALSE(r.when_matched_delete);
    REQUIRE(r.when_not_matched_values.size() == 2);
}

TEST_CASE("Statement::CreateProcedure with ProcIf/ProcWhile control flow round-trips", "[ast_json]") {
    Statement proc_while = Statement::ProcWhile{
        std::optional<std::string>("lbl"),
        CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{"x"}), Operator::Lt, ConditionValue(ConditionValue::Literal{"10"})}}),
        {Statement(Statement::ProcSet{"x", ArithExpr(ArithExpr::Add{std::make_unique<ArithExpr>(ArithExpr::Col{"x"}),
                                                                     std::make_unique<ArithExpr>(ArithExpr::Num{"1"})})})}};

    Statement proc_if = Statement::ProcIf{
        CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{"x"}), Operator::Gt, ConditionValue(ConditionValue::Literal{"0"})}}),
        {proc_while},
        {},
        std::vector<Statement>{Statement(Statement::ProcLeave{std::optional<std::string>("lbl")})}};

    Statement proc = Statement::CreateProcedure{"loop_proc", {{"IN", "x", "INT"}}, {proc_if}};

    Statement back = roundtrip(proc);
    auto& cp = std::get<Statement::CreateProcedure>(back.data);
    REQUIRE(cp.name == "loop_proc");
    REQUIRE(cp.params.size() == 1);
    REQUIRE(std::get<0>(cp.params[0]) == "IN");
    REQUIRE(cp.body.size() == 1);
    auto& if_back = std::get<Statement::ProcIf>(cp.body[0].data);
    REQUIRE(if_back.then_body.size() == 1);
    REQUIRE(std::holds_alternative<Statement::ProcWhile>(if_back.then_body[0].data));
    auto& while_back = std::get<Statement::ProcWhile>(if_back.then_body[0].data);
    REQUIRE(while_back.label == std::optional<std::string>("lbl"));
    REQUIRE(while_back.body.size() == 1);
    REQUIRE(if_back.else_body.has_value());
    REQUIRE(std::holds_alternative<Statement::ProcLeave>((*if_back.else_body)[0].data));
}

TEST_CASE("AlterAction::AddForeignKey and DropConstraint round-trip", "[ast_json]") {
    AlterAction a1 = AlterAction::AddForeignKey{
        std::optional<std::string>("fk1"), "dept_id", "rusql.department", "id", FkAction::SetNull, FkAction::Cascade};
    Statement s1 = Statement::AlterTable{"rusql.employee", a1};
    Statement back1 = roundtrip(s1);
    auto& at1 = std::get<Statement::AlterTable>(back1.data);
    auto& fk = std::get<AlterAction::AddForeignKey>(at1.action.data);
    REQUIRE(fk.name == std::optional<std::string>("fk1"));
    REQUIRE(fk.on_delete == FkAction::SetNull);
    REQUIRE(fk.on_update == FkAction::Cascade);

    AlterAction a2 = AlterAction::DropConstraint{"chk_dept"};
    Statement s2 = Statement::AlterTable{"rusql.employee", a2};
    Statement back2 = roundtrip(s2);
    auto& at2 = std::get<Statement::AlterTable>(back2.data);
    REQUIRE(std::get<AlterAction::DropConstraint>(at2.action.data).name == "chk_dept");
}

TEST_CASE("SelectColumn::WinFunc with a frame round-trips", "[ast_json]") {
    SelectColumn::WinFunc wf;
    wf.func = WindowFunc::Lag;
    wf.col = std::optional<std::string>("salary");
    wf.offset = 1;
    wf.partition_by = {"dept_id"};
    wf.order_by = {OrderBy{"id", true}};
    wf.alias = std::optional<std::string>("prev_salary");
    WindowFrame frame;
    frame.unit = FrameUnit::Rows;
    frame.start = FrameBound(FrameBound::Preceding{2});
    frame.end = FrameBound(FrameBound::CurrentRow{});
    wf.frame = frame;

    Statement s = Statement::Select{
        "rusql.employee", std::nullopt, {SelectColumn(std::move(wf))}, false, std::nullopt, {}, {},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};

    Statement back = roundtrip(s);
    auto& r = std::get<Statement::Select>(back.data);
    auto& wf_back = std::get<SelectColumn::WinFunc>(r.columns[0].data);
    REQUIRE(wf_back.func == WindowFunc::Lag);
    REQUIRE(wf_back.col == std::optional<std::string>("salary"));
    REQUIRE(wf_back.offset == 1);
    REQUIRE(wf_back.partition_by[0] == "dept_id");
    REQUIRE(wf_back.frame.has_value());
    REQUIRE(wf_back.frame->unit == FrameUnit::Rows);
    REQUIRE(std::holds_alternative<FrameBound::Preceding>(wf_back.frame->start.data));
    REQUIRE(std::get<FrameBound::Preceding>(wf_back.frame->start.data).n == 2);
    REQUIRE(std::holds_alternative<FrameBound::CurrentRow>(wf_back.frame->end.data));
}

TEST_CASE("Bare unit-variant Statements round-trip as plain strings", "[ast_json]") {
    for (auto& s : {Statement(Statement::Begin{}), Statement(Statement::Commit{}), Statement(Statement::ShowTables{}),
                     Statement(Statement::ShowProcessList{})}) {
        nlohmann::json j = s;
        REQUIRE(j.is_string());
        Statement back = roundtrip(s);
        REQUIRE(back.data.index() == s.data.index());
    }
}
