-- Top-K 인덱스 최적화 검증
DROP DATABASE IF EXISTS topk_test;
CREATE DATABASE topk_test;
USE topk_test;

CREATE TABLE employee (
    id     INT AUTO INCREMENT,
    name   VARCHAR(50),
    dept   INT,
    salary DECIMAL(10,2),
    PRIMARY KEY (id)
);

CREATE INDEX idx_salary ON employee (salary);
CREATE INDEX idx_name   ON employee (name);
CREATE INDEX idx_dept_sal ON employee (dept, salary);

INSERT INTO employee (name, dept, salary) VALUES
    ('Alice',  1, 120000),
    ('Bob',    1,  95000),
    ('Carol',  2,  85000),
    ('Dave',   2,  75000),
    ('Eve',    3, 140000),
    ('Frank',  3,  78000),
    ('Grace',  1,  65000),
    ('Henry',  1,  90000),
    ('Iris',   2,  55000),
    ('Jack',   3,  70000),
    ('Karen',  1, 110000),
    ('Liam',   2,  80000);

-- 1. SecondaryRange Top-K ASC
-- 기대: salary >= 80000 인 행 중 가장 낮은 3개 (오름차순)
-- 활성화: SecondaryRange(idx_salary, salary >= 80000) + ORDER BY salary ASC LIMIT 3
EXPLAIN SELECT name, salary FROM employee WHERE salary >= 80000 ORDER BY salary ASC LIMIT 3;
SELECT name, salary FROM employee WHERE salary >= 80000 ORDER BY salary ASC LIMIT 3;
-- 예상: Liam(80000), Carol(85000), Henry(90000)

-- 2. SecondaryRange Top-K DESC
-- 기대: salary >= 80000 인 행 중 가장 높은 3개 (내림차순)
EXPLAIN SELECT name, salary FROM employee WHERE salary >= 80000 ORDER BY salary DESC LIMIT 3;
SELECT name, salary FROM employee WHERE salary >= 80000 ORDER BY salary DESC LIMIT 3;
-- 예상: Eve(140000), Alice(120000), Karen(110000)

-- 3. SecondaryBetween Top-K ASC
-- 기대: salary BETWEEN 70000 AND 100000 중 낮은 3개
EXPLAIN SELECT name, salary FROM employee WHERE salary BETWEEN 70000 AND 100000 ORDER BY salary ASC LIMIT 3;
SELECT name, salary FROM employee WHERE salary BETWEEN 70000 AND 100000 ORDER BY salary ASC LIMIT 3;
-- 예상: 70000(Jack), 75000(Dave), 78000(Frank)

-- 4. SecondaryBetween Top-K DESC
EXPLAIN SELECT name, salary FROM employee WHERE salary BETWEEN 70000 AND 100000 ORDER BY salary DESC LIMIT 3;
SELECT name, salary FROM employee WHERE salary BETWEEN 70000 AND 100000 ORDER BY salary DESC LIMIT 3;
-- 예상: 95000(Bob), 90000(Henry), 85000(Carol)

-- 5. CompositeIndexPrefix Top-K ASC
-- 기대: dept=1 인 행 중 salary 낮은 3개
EXPLAIN SELECT name, dept, salary FROM employee WHERE dept = 1 ORDER BY salary ASC LIMIT 3;
SELECT name, dept, salary FROM employee WHERE dept = 1 ORDER BY salary ASC LIMIT 3;
-- 예상: Grace(65000), Bob(95000), Henry(90000) → 실제 정렬: 65000, 90000, 95000

-- 6. CompositeIndexPrefix Top-K DESC
-- 기대: dept=1 인 행 중 salary 높은 3개
EXPLAIN SELECT name, dept, salary FROM employee WHERE dept = 1 ORDER BY salary DESC LIMIT 3;
SELECT name, dept, salary FROM employee WHERE dept = 1 ORDER BY salary DESC LIMIT 3;
-- 예상: Alice(120000), Karen(110000), Bob(95000)

-- 7. SecondaryLikePrefix Top-K ASC
EXPLAIN SELECT name, salary FROM employee WHERE name LIKE 'G%' OR name LIKE 'A%' OR name LIKE 'B%' OR name LIKE 'C%';
-- (name 인덱스 있음) LIKE 단일 조건 테스트:
EXPLAIN SELECT name, salary FROM employee WHERE name LIKE 'A%' ORDER BY name ASC LIMIT 2;
SELECT name, salary FROM employee WHERE name LIKE 'A%' ORDER BY name ASC LIMIT 2;

-- Cleanup
DROP DATABASE topk_test;
