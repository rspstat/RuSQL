#include "catch.hpp"
#include "engine/parser/ast.hpp"

using namespace engine;

TEST_CASE("Statement variants construct like Rust enum variants", "[ast]") {
    Statement s = Statement::CreateTable{"employee", {}, true, {"id"}, {}};
    REQUIRE(std::holds_alternative<Statement::CreateTable>(s.data));
    REQUIRE(std::get<Statement::CreateTable>(s.data).name == "employee");
    REQUIRE(std::get<Statement::CreateTable>(s.data).if_not_exists);
}

TEST_CASE("ArithExpr recursive tree builds and copies deeply", "[ast]") {
    // salary + 100
    ArithExpr expr = ArithExpr::Add{
        std::make_unique<ArithExpr>(ArithExpr::Col{"salary"}),
        std::make_unique<ArithExpr>(ArithExpr::Num{"100"}),
    };

    ArithExpr copy = expr; // must deep-copy, not alias the unique_ptr

    auto& copy_add = std::get<ArithExpr::Add>(copy.data);
    // Mutate the copy's left child; original must be unaffected.
    std::get<ArithExpr::Col>(copy_add.lhs->data).name = "bonus";

    auto& orig_add = std::get<ArithExpr::Add>(expr.data);
    REQUIRE(std::get<ArithExpr::Col>(orig_add.lhs->data).name == "salary");
    REQUIRE(std::get<ArithExpr::Col>(copy_add.lhs->data).name == "bonus");
}

TEST_CASE("CondExpr And/Or/Not tree copies deeply", "[ast]") {
    Condition leftCond{ArithExpr(ArithExpr::Col{"age"}), Operator::Gte, ConditionValue(ConditionValue::Literal{"18"})};
    Condition rightCond{ArithExpr(ArithExpr::Col{"age"}), Operator::Lte, ConditionValue(ConditionValue::Literal{"65"})};

    CondExpr expr = CondExpr::And{
        std::make_unique<CondExpr>(CondExpr::Leaf{leftCond}),
        std::make_unique<CondExpr>(CondExpr::Leaf{rightCond}),
    };

    CondExpr copy = expr;
    auto& copy_and = std::get<CondExpr::And>(copy.data);
    std::get<CondExpr::Leaf>(copy_and.lhs->data).condition.op = Operator::Gt;

    auto& orig_and = std::get<CondExpr::And>(expr.data);
    REQUIRE(std::get<CondExpr::Leaf>(orig_and.lhs->data).condition.op == Operator::Gte);
    REQUIRE(std::get<CondExpr::Leaf>(copy_and.lhs->data).condition.op == Operator::Gt);
}

TEST_CASE("Statement holding a boxed subquery copies independently", "[ast]") {
    // EXPLAIN SELECT * FROM t
    Statement inner = Statement::Select{
        "t", std::nullopt, {SelectColumn(SelectColumn::All{})}, false, std::nullopt, {}, {}, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt, false, false};
    Statement outer = Statement::Explain{std::make_unique<Statement>(inner)};

    Statement copy = outer;
    auto& copy_inner = std::get<Statement::Select>(std::get<Statement::Explain>(copy.data).inner->data);
    copy_inner.table = "other_table";

    auto& orig_inner = std::get<Statement::Select>(std::get<Statement::Explain>(outer.data).inner->data);
    REQUIRE(orig_inner.table == "t");
    REQUIRE(copy_inner.table == "other_table");
}

TEST_CASE("Statement body vectors (procedures) copy independently", "[ast]") {
    Statement proc = Statement::CreateProcedure{
        "double_val",
        {{"IN", "x", "INT"}},
        {Statement(Statement::ProcSet{"result", ArithExpr(ArithExpr::Num{"2"})})},
    };

    Statement copy = proc;
    std::get<Statement::CreateProcedure>(copy.data).body.push_back(Statement(Statement::Commit{}));

    REQUIRE(std::get<Statement::CreateProcedure>(proc.data).body.size() == 1);
    REQUIRE(std::get<Statement::CreateProcedure>(copy.data).body.size() == 2);
}
