"""Intent generators (produce a SQL query + the slots needed to phrase it) and the
Korean/English natural-language phrase templates for each intent type.

Each intent function takes (schema, table, rng) and returns a QueryInstance or None
(when the table doesn't have a column suitable for that intent). generate.py calls
every intent against every table in every schema and expands each QueryInstance into
one training row per phrasing (so one SQL query yields several NL question variants).
"""

import random
from dataclasses import dataclass, field

from korean import eul_reul, eun_neun, gwa_wa, i_ga, ro_euro
from schemas import Column, Schema, Table
from term_dict import label_en, label_en_plural, label_ko

AGG_LABELS = {
    "SUM": ("total", "합계"),
    "AVG": ("average", "평균"),
    "MAX": ("maximum", "최댓값"),
    "MIN": ("minimum", "최솟값"),
}


@dataclass
class QueryInstance:
    sql: str
    ko: list[str] = field(default_factory=list)
    en: list[str] = field(default_factory=list)


def _label_col(table: Table) -> Column | None:
    """Pick a human-readable column for display in join phrasing (prefers name/title)."""
    for preferred in ("name", "title"):
        for c in table.columns:
            if c.name == preferred:
                return c
    for c in table.columns:
        if c.kind == "text":
            return c
    return None


def _text_or_enum_cols(table: Table) -> list[Column]:
    return [c for c in table.columns if c.kind in ("text", "enum") and (c.samples or c.enum_values)]


def _number_cols(table: Table) -> list[Column]:
    return [c for c in table.columns if c.kind == "number"]


def _date_cols(table: Table) -> list[Column]:
    return [c for c in table.columns if c.kind == "date"]


def _sample_value(col: Column, rng: random.Random) -> str:
    if col.kind == "enum":
        return rng.choice(col.enum_values)
    if col.samples:
        return rng.choice(col.samples)
    return "sample"


def _number_threshold(col: Column, rng: random.Random) -> int:
    return rng.choice([10, 50, 100, 500, 1000])


_COMPARISON_OPS = [(">", "큰", "greater than"), ("<", "작은", "less than"), (">=", "이상인", "at least")]


def _first_token(value: str) -> str:
    return value.split()[0] if value.split() else value


# ---------------------------------------------------------------------------
# Intents
# ---------------------------------------------------------------------------

def gen_select_all(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    return QueryInstance(
        sql=f"SELECT * FROM {table.name};",
        ko=[f"{t_ko} 전체 목록을 보여줘", f"{t_ko} 다 보여줘", f"{t_ko} 전체를 조회해줘", f"{t_ko} 목록 좀 보여줄래?"],
        en=[f"Show me all {t_pl}", f"List every {label_en(table.name)}", f"Give me the full list of {t_pl}",
            f"What {t_pl} do we have?"],
    )


def gen_filter_eq(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _text_or_enum_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    val = _sample_value(col, rng)
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} WHERE {col.name} = '{val}';",
        ko=[f"{t_ko} 중에서 {i_ga(c_ko)} {val}인 것들 보여줘",
            f"{i_ga(c_ko)} {val}인 {t_ko} 목록 알려줘",
            f"{t_ko}에서 {i_ga(c_ko)} {val}인 항목을 찾아줘",
            f"{i_ga(c_ko)} {val}인 {t_ko} 있어?"],
        en=[f"Show {t_pl} where {c_en} is '{val}'",
            f"Which {t_pl} have {c_en} equal to '{val}'?",
            f"List {t_pl} with {c_en} = '{val}'",
            f"Find the {label_en(table.name)} whose {c_en} is '{val}'"],
    )


def gen_filter_numeric(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _number_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    val = _number_threshold(col, rng)
    op, op_ko, op_en = rng.choice(_COMPARISON_OPS)
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} WHERE {col.name} {op} {val};",
        ko=[f"{i_ga(c_ko)} {val}보다 {op_ko} {eul_reul(t_ko)} 보여줘",
            f"{t_ko} 중 {i_ga(c_ko)} {val} {op_ko} 것들 알려줘",
            f"{i_ga(c_ko)} {val} {op_ko} {t_ko}만 추려줘"],
        en=[f"Show {t_pl} where {c_en} is {op_en} {val}",
            f"List {t_pl} whose {c_en} is {op_en} {val}",
            f"Find {t_pl} with a {c_en} {op_en} {val}"],
    )


def gen_sort_limit(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _number_cols(table) + _date_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    n = rng.choice([3, 5, 10])
    desc = rng.choice([True, False])
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    dir_ko = "높은" if desc else "낮은"
    dir_en = "highest" if desc else "lowest"
    order = "DESC" if desc else "ASC"
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} ORDER BY {col.name} {order} LIMIT {n};",
        ko=[f"{i_ga(c_ko)} 가장 {dir_ko} {t_ko} {n}개를 보여줘",
            f"{c_ko} 기준으로 {dir_ko} 순으로 {t_ko} {n}개만 보여줘",
            f"{t_ko} 중 {i_ga(c_ko)} 제일 {dir_ko} {n}개만 알려줘"],
        en=[f"Show the {n} {t_pl} with the {dir_en} {c_en}",
            f"List top {n} {t_pl} sorted by {c_en} ({'descending' if desc else 'ascending'})",
            f"What are the {n} {t_pl} with the {dir_en} {c_en}?"],
    )


def gen_count_all(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    return QueryInstance(
        sql=f"SELECT COUNT(*) FROM {table.name};",
        ko=[f"{eun_neun(t_ko)} 총 몇 개 있어?", f"{t_ko} 전체 개수를 알려줘"],
        en=[f"How many {t_pl} are there?", f"Count all {t_pl}"],
    )


def gen_count_filtered(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _text_or_enum_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    val = _sample_value(col, rng)
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT COUNT(*) FROM {table.name} WHERE {col.name} = '{val}';",
        ko=[f"{i_ga(c_ko)} {val}인 {t_ko}는 몇 개야?", f"{i_ga(c_ko)} {val}인 {t_ko} 개수를 알려줘"],
        en=[f"How many {t_pl} have {c_en} equal to '{val}'?", f"Count {t_pl} where {c_en} is '{val}'"],
    )


def gen_agg_numeric(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _number_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    agg = rng.choice(list(AGG_LABELS.keys()))
    agg_en, agg_ko = AGG_LABELS[agg]
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT {agg}({col.name}) FROM {table.name};",
        ko=[f"{t_ko}의 {c_ko} {eul_reul(agg_ko)} 알려줘", f"{t_ko} {c_ko}의 {eun_neun(agg_ko)}?"],
        en=[f"What is the {agg_en} {c_en} of {t_pl}?", f"Show the {agg_en} {c_en} across all {t_pl}"],
    )


def gen_group_agg(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    group_cols = [c for c in table.columns if c.kind == "enum"]
    num_cols = _number_cols(table)
    if not group_cols or not num_cols:
        return None
    gcol = rng.choice(group_cols)
    ncol = rng.choice(num_cols)
    agg = rng.choice(list(AGG_LABELS.keys()))
    agg_en, agg_ko = AGG_LABELS[agg]
    t_ko = label_ko(table.name)
    g_en, g_ko = label_en(gcol.name), label_ko(gcol.name)
    n_en, n_ko = label_en(ncol.name), label_ko(ncol.name)
    return QueryInstance(
        sql=f"SELECT {gcol.name}, {agg}({ncol.name}) FROM {table.name} GROUP BY {gcol.name};",
        ko=[f"{g_ko}별로 {n_ko} {eul_reul(agg_ko)} 보여줘", f"{eul_reul(t_ko)} {g_ko}별로 묶어서 {n_ko} {eul_reul(agg_ko)} 알려줘"],
        en=[f"Show the {agg_en} {n_en} grouped by {g_en}", f"Break down {agg_en} {n_en} by {g_en}"],
    )


def gen_join_filter(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    fk_cols = [c for c in table.columns if c.kind == "fk"]
    if not fk_cols:
        return None
    fkcol = rng.choice(fk_cols)
    ref_table_name = fkcol.fk[0]
    if ref_table_name not in [t.name for t in schema.tables]:
        return None
    ref_table = schema.table(ref_table_name)
    ref_filter_cols = _text_or_enum_cols(ref_table)
    if not ref_filter_cols:
        return None
    fcol = rng.choice(ref_filter_cols)
    val = _sample_value(fcol, rng)
    a_ko = label_ko(table.name)
    a_pl = label_en_plural(table.name)
    b_en, b_ko = label_en(ref_table.name), label_ko(ref_table.name)
    fc_en, fc_ko = label_en(fcol.name), label_ko(fcol.name)
    return QueryInstance(
        sql=(f"SELECT {table.name}.* FROM {table.name} JOIN {ref_table.name} "
             f"ON {table.name}.{fkcol.name} = {ref_table.name}.id "
             f"WHERE {ref_table.name}.{fcol.name} = '{val}';"),
        ko=[f"{gwa_wa(b_ko)} 연결된 {a_ko} 중에서 {b_ko}의 {i_ga(fc_ko)} {val}인 것들 보여줘",
            f"{b_ko}의 {i_ga(fc_ko)} {val}인 {eul_reul(a_ko)} 알려줘"],
        en=[f"Show {a_pl} where the associated {b_en}'s {fc_en} is '{val}'",
            f"List {a_pl} whose {b_en} has {fc_en} = '{val}'"],
    )


def gen_join_count_group(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    fk_cols = [c for c in table.columns if c.kind == "fk"]
    if not fk_cols:
        return None
    fkcol = rng.choice(fk_cols)
    ref_table_name = fkcol.fk[0]
    if ref_table_name not in [t.name for t in schema.tables]:
        return None
    ref_table = schema.table(ref_table_name)
    label_column = _label_col(ref_table)
    if not label_column:
        return None
    a_ko = label_ko(table.name)
    a_pl = label_en_plural(table.name)
    b_en, b_ko = label_en(ref_table.name), label_ko(ref_table.name)
    lc = label_column.name
    return QueryInstance(
        sql=(f"SELECT {ref_table.name}.{lc}, COUNT(*) FROM {table.name} "
             f"JOIN {ref_table.name} ON {table.name}.{fkcol.name} = {ref_table.name}.id "
             f"GROUP BY {ref_table.name}.{lc};"),
        ko=[f"{b_ko}별로 {a_ko} 개수를 보여줘", f"각 {b_ko}마다 {eul_reul(a_ko)} 몇 개씩 갖고 있는지 알려줘"],
        en=[f"Show the number of {a_pl} per {b_en}", f"How many {a_pl} does each {b_en} have?"],
    )


def gen_filter_like(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = [c for c in table.columns if c.kind == "text" and c.samples]
    if not cols:
        return None
    col = rng.choice(cols)
    substr = _first_token(rng.choice(col.samples))
    ig_particle = i_ga(substr)[len(substr):]
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} WHERE {col.name} LIKE '%{substr}%';",
        ko=[f"{t_ko} 중에서 {c_ko}에 '{substr}'{ig_particle} 포함된 것들 보여줘",
            f"{c_ko}에 '{substr}'{ig_particle} 들어간 {eul_reul(t_ko)} 찾아줘"],
        en=[f"Show {t_pl} where {c_en} contains '{substr}'",
            f"Find {t_pl} whose {c_en} includes '{substr}'"],
    )


def gen_multi_filter_and(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    eq_cols = _text_or_enum_cols(table)
    num_cols = _number_cols(table)
    if not eq_cols or not num_cols:
        return None
    col1 = rng.choice(eq_cols)
    col2 = rng.choice(num_cols)
    val1 = _sample_value(col1, rng)
    val2 = _number_threshold(col2, rng)
    op, op_ko, op_en = rng.choice(_COMPARISON_OPS)
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c1_en, c1_ko = label_en(col1.name), label_ko(col1.name)
    c2_en, c2_ko = label_en(col2.name), label_ko(col2.name)
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} WHERE {col1.name} = '{val1}' AND {col2.name} {op} {val2};",
        ko=[f"{t_ko} 중에서 {i_ga(c1_ko)} {val1}이고 {i_ga(c2_ko)} {val2}보다 {op_ko} 것들 보여줘",
            f"{c1_ko}는 {val1}, {c2_ko}은(는) {val2} {op_ko} {eul_reul(t_ko)} 알려줘"],
        en=[f"Show {t_pl} where {c1_en} is '{val1}' and {c2_en} is {op_en} {val2}",
            f"List {t_pl} with {c1_en} = '{val1}' and {c2_en} {op_en} {val2}"],
    )


def gen_null_check(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _date_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    is_null = rng.choice([True, False])
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    if is_null:
        return QueryInstance(
            sql=f"SELECT * FROM {table.name} WHERE {col.name} IS NULL;",
            ko=[f"{i_ga(c_ko)} 비어있는 {eul_reul(t_ko)} 보여줘", f"{i_ga(c_ko)} 아직 없는 {t_ko} 목록을 알려줘"],
            en=[f"Show {t_pl} where {c_en} is empty", f"List {t_pl} with no {c_en}"],
        )
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} WHERE {col.name} IS NOT NULL;",
        ko=[f"{i_ga(c_ko)} 채워진 {eul_reul(t_ko)} 보여줘", f"{i_ga(c_ko)} 있는 {t_ko}만 알려줘"],
        en=[f"Show {t_pl} where {c_en} is set", f"List {t_pl} that have a {c_en}"],
    )


def gen_distinct(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = [c for c in table.columns if c.kind == "enum"] or [c for c in table.columns if c.kind == "text" and c.samples]
    if not cols:
        return None
    col = rng.choice(cols)
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT DISTINCT {col.name} FROM {table.name};",
        ko=[f"{t_ko}에 있는 {c_ko} 값을 전부 보여줘", f"{t_ko}에 어떤 {i_ga(c_ko)} 있는지 알려줘"],
        en=[f"Show all distinct {c_en} values in {t_pl}", f"What {c_en} values exist in {t_pl}?"],
    )


def gen_between(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _number_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    low = _number_threshold(col, rng)
    high = low + rng.choice([10, 50, 100])
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} WHERE {col.name} BETWEEN {low} AND {high};",
        ko=[f"{i_ga(c_ko)} {low}에서 {high} 사이인 {eul_reul(t_ko)} 보여줘",
            f"{t_ko} 중 {i_ga(c_ko)} {low}~{high} 범위인 것들 알려줘"],
        en=[f"Show {t_pl} where {c_en} is between {low} and {high}",
            f"List {t_pl} with {c_en} ranging from {low} to {high}"],
    )


def gen_order_no_limit(schema: Schema, table: Table, rng: random.Random) -> QueryInstance | None:
    cols = _number_cols(table) + _date_cols(table)
    if not cols:
        return None
    col = rng.choice(cols)
    desc = rng.choice([True, False])
    t_ko = label_ko(table.name)
    t_pl = label_en_plural(table.name)
    c_en, c_ko = label_en(col.name), label_ko(col.name)
    dir_ko = "내림차순" if desc else "오름차순"
    order = "DESC" if desc else "ASC"
    return QueryInstance(
        sql=f"SELECT * FROM {table.name} ORDER BY {col.name} {order};",
        ko=[f"{eul_reul(t_ko)} {c_ko} 기준 {dir_ko}으로 정렬해서 보여줘", f"{ro_euro(c_ko)} 정렬한 {t_ko} 전체를 보여줘"],
        en=[f"Show {t_pl} sorted by {c_en} ({'descending' if desc else 'ascending'})",
            f"List all {t_pl} ordered by {c_en}"],
    )


INTENTS = [
    gen_select_all, gen_filter_eq, gen_filter_numeric, gen_sort_limit,
    gen_count_all, gen_count_filtered, gen_agg_numeric, gen_group_agg,
    gen_join_filter, gen_join_count_group,
    gen_filter_like, gen_multi_filter_and, gen_null_check, gen_distinct,
    gen_between, gen_order_no_limit,
]
