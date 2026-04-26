-- test_limit_offset_cte.sql

DROP TABLE IF EXISTS t_archive;
DROP TABLE IF EXISTS t_src;

CREATE TABLE t_src (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50),
    score INT
);

INSERT INTO t_src (name, score) VALUES
    ('Alice', 90), ('Bob', 75), ('Carol', 88),
    ('Dave', 60),  ('Eve', 95), ('Frank', 72);

-- 1. LIMIT only (regression)
SELECT id, name FROM t_src ORDER BY id LIMIT 3;

-- 2. LIMIT OFFSET (page 2: rows 3-4)
SELECT id, name FROM t_src ORDER BY id LIMIT 2 OFFSET 2;

-- 3. OFFSET only larger than result (empty)
SELECT id, name FROM t_src ORDER BY id LIMIT 2 OFFSET 10;

-- 4. ORDER BY + LIMIT OFFSET (top 2 scorers, skip first)
SELECT name, score FROM t_src ORDER BY score DESC LIMIT 2 OFFSET 1;

-- 5. INSERT ... SELECT (full copy)
CREATE TABLE t_archive (
    id    INT PRIMARY KEY,
    name  VARCHAR(50),
    score INT
);
INSERT INTO t_archive SELECT id, name, score FROM t_src WHERE score >= 80;
SELECT id, name, score FROM t_archive ORDER BY id;

-- 6. INSERT ... SELECT with column mapping
INSERT INTO t_archive (id, name, score)
    SELECT id, name, score FROM t_src WHERE score < 80 ORDER BY id LIMIT 2;
SELECT id, name, score FROM t_archive ORDER BY score DESC;

-- 7. CTE simple
WITH top_scorers AS (
    SELECT name, score FROM t_src WHERE score >= 80
)
SELECT name, score FROM top_scorers ORDER BY score DESC;

-- 8. CTE with aggregate
WITH dept_avg AS (
    SELECT score FROM t_src WHERE score >= 70
)
SELECT COUNT(*) FROM dept_avg;

-- 9. Multiple CTEs
WITH highs AS (
    SELECT id, name, score FROM t_src WHERE score >= 85
),
lows AS (
    SELECT id, name, score FROM t_src WHERE score < 75
)
SELECT name, score FROM highs
UNION ALL
SELECT name, score FROM lows
ORDER BY score DESC;

-- 10. CTE used in INSERT ... SELECT
WITH passing AS (
    SELECT id, name, score FROM t_src WHERE score >= 88 AND id NOT IN (SELECT id FROM t_archive)
)
INSERT INTO t_archive SELECT id, name, score FROM passing;
SELECT id, name, score FROM t_archive ORDER BY id;

-- CLEANUP
DROP TABLE t_archive;
DROP TABLE t_src;
