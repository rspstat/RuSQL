-- test_new_features.sql
-- Tests: CASE WHEN, IF(), UNION/UNION ALL, JOIN ON with complex conditions, NULL semantics

DROP TABLE IF EXISTS students;
DROP TABLE IF EXISTS scores;
DROP TABLE IF EXISTS grade_ranges;

CREATE TABLE students (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL,
    dept VARCHAR(20)
);

CREATE TABLE scores (
    student_id INT,
    subject    VARCHAR(30),
    score      INT
);

CREATE TABLE grade_ranges (
    min_score INT,
    max_score INT,
    grade     VARCHAR(5)
);

INSERT INTO students (name, dept) VALUES
    ('Alice', 'CS'),
    ('Bob',   'CS'),
    ('Carol', 'Math'),
    ('Dave',  NULL);

INSERT INTO scores VALUES
    (1, 'Math',    92),
    (1, 'English', 78),
    (2, 'Math',    55),
    (2, 'English', 88),
    (3, 'Math',    73),
    (4, 'Math',    40);

INSERT INTO grade_ranges VALUES
    (90, 100, 'A'),
    (80,  89, 'B'),
    (70,  79, 'C'),
    (60,  69, 'D'),
    ( 0,  59, 'F');

-- 1. CASE WHEN (simple)
SELECT
    id,
    name,
    CASE
        WHEN dept = 'CS'   THEN 'Computer Science'
        WHEN dept = 'Math' THEN 'Mathematics'
        ELSE 'Unknown'
    END AS department
FROM students;

-- 2. IF() with comparison operator
SELECT
    student_id,
    subject,
    score,
    IF(score >= 60, 'PASS', 'FAIL') AS result
FROM scores
ORDER BY student_id, subject;

-- 3. IF() with NULL check via IS NULL (column IS NULL is a CondExpr)
SELECT id, name, IF(dept IS NULL, 'No dept', dept) AS dept_label FROM students;

-- 4. JOIN ON with AND (equality + equality) — regression test
SELECT s.name, sc.subject, sc.score
FROM students s
JOIN scores sc ON s.id = sc.student_id AND sc.score > 70
ORDER BY s.name, sc.subject;

-- 5. JOIN ON with complex condition (non-equality): BETWEEN via grade_ranges
SELECT sc.student_id, sc.score, gr.grade
FROM scores sc
JOIN grade_ranges gr ON sc.score >= gr.min_score AND sc.score <= gr.max_score
ORDER BY sc.student_id, sc.score;

-- 6. UNION
SELECT id, name FROM students WHERE dept = 'CS'
UNION
SELECT id, name FROM students WHERE dept = 'Math';

-- 7. UNION ALL (includes duplicates if any)
SELECT subject FROM scores WHERE score >= 80
UNION ALL
SELECT subject FROM scores WHERE score >= 90;

-- 8. UNION deduplication (both sides return same row)
SELECT name FROM students WHERE id = 1
UNION
SELECT name FROM students WHERE name = 'Alice';

-- 9. NULL semantics: WHERE col = value should not match NULL rows
SELECT id, name FROM students WHERE dept = 'CS';

-- 10. CASE WHEN with condition tree (AND)
SELECT
    student_id,
    score,
    CASE
        WHEN score >= 90 THEN 'Excellent'
        WHEN score >= 70 AND score < 90 THEN 'Good'
        ELSE 'Needs Improvement'
    END AS evaluation
FROM scores
ORDER BY student_id;

-- CLEANUP
DROP TABLE grade_ranges;
DROP TABLE scores;
DROP TABLE students;
