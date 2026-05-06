# DIFF — 변경 이력

## 2026-05-07

### 버그 수정

#### 1. `exec_insert` — 보조 인덱스 재빌드 누락 (executor.rs)

**증상**: `CREATE INDEX` → `INSERT` → `UNION SELECT` (보조 인덱스 경유) 시 0 rows 반환.

**원인**: `exec_insert`가 PK 인덱스 및 복합 인덱스는 갱신했지만, 단일 컬럼 보조 인덱스(`s.indexes`)를 재빌드하지 않았음. 인덱스가 빈 B+Tree 상태로 남아 `SecondaryPoint` 탐색 시 항상 `None` 반환.

**수정**: `exec_insert` 내 `sort_by_pk` 호출 직후 `rebuild_secondary_indexes(s, &table, &rows)` 추가.

```rust
self.sort_by_pk(s, &table);
let rows = s.tables.get(&table).unwrap().clone();
self.rebuild_secondary_indexes(s, &table, &rows);
s.buffer_pool.write_page(&table, rows);
```

---

#### 2. `is_covering_access` — 복합 인덱스 커버링 미인식 (planner.rs)

**증상**: `EXPLAIN SELECT dept_id, salary FROM emp WHERE dept_id = 1` 에서 `(Covering)` 미표시.

**원인**: `is_covering_access`가 단일 컬럼 인덱스만 검사 (선택 컬럼 전체가 인덱스 컬럼 하나와 일치하는지만 확인). 복합 인덱스(`idx_ds ON emp(dept_id, salary)`)로 커버 가능한 경우를 무시.

**수정**: 인스턴스 메서드로 변경 후 복합 인덱스 커버리지 검사 추가. 선택 컬럼 전체가 복합 인덱스의 컬럼 집합에 속하면 `is_covering = true`.

```rust
fn is_covering_access(&self, access: &AccessPath, columns: &[SelectColumn], table: &str) -> bool {
    // ... (단일 컬럼 검사 후)
    self.composite_indexes.values().any(|ci| {
        ci.table == table && selected.iter().all(|sc| ci.columns.iter().any(|ic| ic == sc))
    })
}
```

---

#### 3. GROUP BY + ORDER BY 집계 별칭 정렬 오작동 (executor.rs)

**증상**: `GROUP BY grade HAVING n >= 1 ORDER BY avg_sal DESC` 에서 `avg_sal` 기준 정렬이 적용되지 않음. 삽입 순서대로 반환.

**원인**: ORDER BY 정렬이 GROUP BY 실행 *전* 원시 행에 적용되었음. 집계 결과 행(`group_rows`)에는 `avg_sal` 별칭이 존재하지 않으므로 정렬이 무효.

**수정**: HAVING 필터 *후* `group_rows`에 ORDER BY 정렬 패스 추가. 숫자 비교 우선 적용.

```rust
if !order_by.is_empty() {
    group_rows.sort_by(|a, b| {
        for ord in &order_by {
            let av = Self::get_col(a, &ord.column).cloned().unwrap_or_default();
            let bv = Self::get_col(b, &ord.column).cloned().unwrap_or_default();
            let cmp = match (av.parse::<f64>(), bv.parse::<f64>()) {
                (Ok(af), Ok(bf)) => af.partial_cmp(&bf).unwrap_or(Ordering::Equal),
                _ => av.cmp(&bv),
            };
            let cmp = if ord.ascending { cmp } else { cmp.reverse() };
            if cmp != Ordering::Equal { return cmp; }
        }
        Ordering::Equal
    });
}
```

---

#### 4. `expect_col_ref` — SQL 키워드를 컬럼명으로 파싱 불가 (parser.rs)

**증상**: `ORDER BY avg DESC`, `GROUP BY count` 등 SQL 예약어를 컬럼/별칭 이름으로 사용 시 `Parse Error: Expected identifier, got Some(Avg)`.

**원인**: `expect_col_ref`가 `expect_ident`를 호출하는데, `expect_ident`는 `Token::Ident`만 수락. `avg`, `count`, `sum` 등 집계 함수명은 별도 키워드 토큰으로 렉싱됨.

**수정**: `expect_any_ident` 함수 추가 (키워드 토큰 → 문자열 변환). `expect_col_ref`가 `expect_any_ident`를 사용하도록 변경.

```rust
fn expect_any_ident(&mut self) -> Result<String, String> {
    match self.advance() {
        Some(Token::Ident(s))  => Ok(s.clone()),
        Some(Token::Count)     => Ok("count".to_string()),
        Some(Token::Sum)       => Ok("sum".to_string()),
        Some(Token::Avg)       => Ok("avg".to_string()),
        Some(Token::Min)       => Ok("min".to_string()),
        Some(Token::Max)       => Ok("max".to_string()),
        Some(Token::Now)       => Ok("now".to_string()),
        Some(Token::Date)      => Ok("date".to_string()),
        Some(Token::Key)       => Ok("key".to_string()),
        Some(Token::Set)       => Ok("set".to_string()),
        Some(Token::Index)     => Ok("index".to_string()),
        Some(Token::View)      => Ok("view".to_string()),
        other => Err(format!("Expected identifier, got {:?}", other)),
    }
}
```

---

### 테스트

`test/test_full.sql` — 260개 구문 전체 통과 (의도된 오류 3개: ENUM/SET 유효성 위반 제외).

---

## 이전 변경 이력

git log 참조.
