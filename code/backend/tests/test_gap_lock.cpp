#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;

namespace {
CondExpr leaf(const std::string& col, Operator op, const std::string& val) {
    return CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{col}), op, ConditionValue(ConditionValue::Literal{val})}});
}

CondExpr between(const std::string& col, const std::string& lo, const std::string& hi) {
    return CondExpr(
        CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{col}), Operator::Between, ConditionValue(ConditionValue::Between{lo, hi})}});
}

CondExpr and_(CondExpr lhs, CondExpr rhs) {
    return CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(lhs)), std::make_unique<CondExpr>(std::move(rhs))});
}

CondExpr or_(CondExpr lhs, CondExpr rhs) {
    return CondExpr(CondExpr::Or{std::make_unique<CondExpr>(std::move(lhs)), std::make_unique<CondExpr>(std::move(rhs))});
}
} // namespace

TEST_CASE("extract_pk_gap_range: no condition is fully unbounded", "[gap_lock]") {
    auto range = Executor::extract_pk_gap_range(std::nullopt, "id");
    REQUIRE_FALSE(range.lo.has_value());
    REQUIRE_FALSE(range.hi.has_value());
}

TEST_CASE("extract_pk_gap_range: Eq pins lo==hi, both inclusive", "[gap_lock]") {
    auto range = Executor::extract_pk_gap_range(leaf("id", Operator::Eq, "5"), "id");
    REQUIRE(range.lo == "5");
    REQUIRE(range.hi == "5");
    REQUIRE(range.lo_inclusive);
    REQUIRE(range.hi_inclusive);
}

TEST_CASE("extract_pk_gap_range: Gt/Lt are exclusive bounds", "[gap_lock]") {
    auto gt = Executor::extract_pk_gap_range(leaf("id", Operator::Gt, "5"), "id");
    REQUIRE(gt.lo == "5");
    REQUIRE_FALSE(gt.lo_inclusive);
    REQUIRE_FALSE(gt.hi.has_value());

    auto lt = Executor::extract_pk_gap_range(leaf("id", Operator::Lt, "20"), "id");
    REQUIRE(lt.hi == "20");
    REQUIRE_FALSE(lt.hi_inclusive);
    REQUIRE_FALSE(lt.lo.has_value());
}

TEST_CASE("extract_pk_gap_range: Gte/Lte are inclusive bounds", "[gap_lock]") {
    auto gte = Executor::extract_pk_gap_range(leaf("id", Operator::Gte, "5"), "id");
    REQUIRE(gte.lo == "5");
    REQUIRE(gte.lo_inclusive);

    auto lte = Executor::extract_pk_gap_range(leaf("id", Operator::Lte, "20"), "id");
    REQUIRE(lte.hi == "20");
    REQUIRE(lte.hi_inclusive);
}

TEST_CASE("extract_pk_gap_range: Between sets both bounds inclusive", "[gap_lock]") {
    auto range = Executor::extract_pk_gap_range(between("id", "10", "20"), "id");
    REQUIRE(range.lo == "10");
    REQUIRE(range.hi == "20");
    REQUIRE(range.lo_inclusive);
    REQUIRE(range.hi_inclusive);
}

TEST_CASE("extract_pk_gap_range: AND-combined Gt+Lte narrows both sides", "[gap_lock]") {
    auto range = Executor::extract_pk_gap_range(and_(leaf("id", Operator::Gt, "5"), leaf("id", Operator::Lte, "20")), "id");
    REQUIRE(range.lo == "5");
    REQUIRE_FALSE(range.lo_inclusive);
    REQUIRE(range.hi == "20");
    REQUIRE(range.hi_inclusive);
}

TEST_CASE("extract_pk_gap_range: top-level OR falls back to unbounded", "[gap_lock]") {
    auto range = Executor::extract_pk_gap_range(or_(leaf("id", Operator::Eq, "5"), leaf("id", Operator::Eq, "100")), "id");
    REQUIRE_FALSE(range.lo.has_value());
    REQUIRE_FALSE(range.hi.has_value());
}

TEST_CASE("extract_pk_gap_range: condition on a non-PK column falls back to unbounded", "[gap_lock]") {
    auto range = Executor::extract_pk_gap_range(leaf("status", Operator::Eq, "active"), "id");
    REQUIRE_FALSE(range.lo.has_value());
    REQUIRE_FALSE(range.hi.has_value());
}

TEST_CASE("gap_range_contains: unbounded range contains anything", "[gap_lock]") {
    GapRange range;
    REQUIRE(Executor::gap_range_contains(range, "0"));
    REQUIRE(Executor::gap_range_contains(range, "999999"));
    REQUIRE(Executor::gap_range_contains(range, "abc"));
}

TEST_CASE("gap_range_contains: numeric comparison respects inclusivity at bounds", "[gap_lock]") {
    GapRange range{"10", "20", true, true};
    REQUIRE_FALSE(Executor::gap_range_contains(range, "9"));
    REQUIRE(Executor::gap_range_contains(range, "10"));
    REQUIRE(Executor::gap_range_contains(range, "15"));
    REQUIRE(Executor::gap_range_contains(range, "20"));
    REQUIRE_FALSE(Executor::gap_range_contains(range, "21"));

    GapRange exclusive{"10", "20", false, false};
    REQUIRE_FALSE(Executor::gap_range_contains(exclusive, "10"));
    REQUIRE_FALSE(Executor::gap_range_contains(exclusive, "20"));
    REQUIRE(Executor::gap_range_contains(exclusive, "15"));
}
