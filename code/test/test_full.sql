-- ═══════════════════════════════════════════════════════════════════════════
-- RustDB 전체 기능 통합 테스트
-- ═══════════════════════════════════════════════════════════════════════════

-- ───────────────────────────────────────────────────────────────────────────
-- [0] SETUP — 멱등 실행: 이전 잔류 데이터 정리
-- ───────────────────────────────────────────────────────────────────────────
DROP USER IF EXISTS 'testuser'@'localhost';
DROP USER IF EXISTS 'readonly'@'%';
DROP USER IF EXISTS 'alice'@'localhost';
DROP DATABASE IF EXISTS shopdb;
DROP DATABASE IF EXISTS hrdb;
DROP DATABASE IF EXISTS logdb;
DROP DATABASE IF EXISTS testdb;

-- ───────────────────────────────────────────────────────────────────────────
-- [1] SHOW DATABASES
-- ───────────────────────────────────────────────────────────────────────────
SHOW DATABASES;

-- ═══════════════════════════════════════════════════════════════════════════
-- DATABASE 1 : hrdb (인사관리)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE DATABASE hrdb;
USE hrdb;

-- ── DDL : CREATE TABLE (FK RESTRICT / CASCADE) ──────────────────────────
CREATE TABLE departments (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE,
    budget INT DEFAULT 0
);
CREATE TABLE employees (
    id         INT PRIMARY KEY AUTO INCREMENT,
    name       VARCHAR(50) NOT NULL,
    dept_id    INT,
    position   VARCHAR(40),
    hire_date  DATE,
    salary     INT CHECK (salary > 0),
    active     INT DEFAULT 1,
    FOREIGN KEY (dept_id) REFERENCES departments(id) ON DELETE SET NULL ON UPDATE CASCADE
);
CREATE TABLE salaries (
    id          INT PRIMARY KEY AUTO INCREMENT,
    employee_id INT NOT NULL,
    amount      INT CHECK (amount > 0),
    grade       ENUM('S1','S2','S3','S4','S5'),
    FOREIGN KEY (employee_id) REFERENCES employees(id) ON DELETE CASCADE
);

-- ── DML : INSERT ─────────────────────────────────────────────────────────
INSERT INTO departments (name, budget) VALUES
    ('Engineering', 500000000),
    ('Marketing',   200000000),
    ('Finance',     300000000),
    ('HR',          150000000);
INSERT INTO employees (name, dept_id, position, hire_date, salary, active) VALUES
    ('Alice',  1, 'Lead Engineer',     '2019-03-15', 9500000, 1),
    ('Bob',    1, 'Senior Engineer',   '2020-07-01', 8500000, 1),
    ('Carol',  2, 'Marketing Manager', '2018-11-20', 7200000, 1),
    ('Dave',   2, 'Marketing Analyst', '2021-04-10', 5800000, 1),
    ('Eve',    3, 'CFO',               '2015-01-05', 12000000, 1),
    ('Frank',  4, 'HR Specialist',     '2022-08-30', 4500000, 0),
    ('Grace',  1, 'Engineer',          '2017-06-12', 9000000, 1),
    ('Henry',  NULL, 'Consultant',     '2023-02-01', 5000000, 1);
INSERT INTO salaries (employee_id, amount, grade) VALUES
    (1, 9500000, 'S4'), (2, 8500000, 'S3'), (3, 7200000, 'S3'),
    (4, 5800000, 'S2'), (5, 12000000, 'S5'), (6, 4500000, 'S1'),
    (7, 9000000, 'S4'), (8, 5000000, 'S1');

-- ── DDL : CREATE INDEX ───────────────────────────────────────────────────
CREATE INDEX idx_emp_dept   ON employees (dept_id);
CREATE INDEX idx_emp_active ON employees (active);
CREATE INDEX idx_sal_emp    ON salaries (employee_id);

-- ── DDL : CREATE VIEW ────────────────────────────────────────────────────
CREATE VIEW v_active_emp AS
    SELECT id, name, dept_id, position FROM employees WHERE active = 1;
CREATE VIEW v_high_earners AS
    SELECT employee_id, amount, grade FROM salaries WHERE amount > 7000000;
CREATE VIEW v_emp_detail AS
    SELECT e.id, e.name, e.position, d.name AS dept, s.amount, s.grade
    FROM employees e
    LEFT JOIN departments d ON e.dept_id = d.id
    LEFT JOIN salaries s ON e.id = s.employee_id;

SHOW TABLES;

-- ── [2] SELECT 기본 / ORDER BY / LIMIT / OFFSET / DISTINCT ──────────────
SELECT id, name, position, salary FROM employees WHERE active = 1 ORDER BY salary DESC;
SELECT id, name FROM employees ORDER BY id LIMIT 3 OFFSET 2;
SELECT DISTINCT dept_id FROM employees ORDER BY dept_id;

-- ── [3] WHERE — 비교 / AND / OR / NOT ───────────────────────────────────
SELECT name, salary FROM employees WHERE salary >= 8000000 AND active = 1;
SELECT name FROM employees WHERE dept_id = 1 OR dept_id = 3;
SELECT name FROM employees WHERE NOT (active = 1);

-- ── [4] IN / NOT IN / BETWEEN / LIKE / IS NULL / IS NOT NULL ────────────
SELECT name FROM employees WHERE dept_id IN (1, 2);
SELECT name FROM employees WHERE dept_id NOT IN (3, 4);
SELECT employee_id, amount FROM salaries WHERE amount BETWEEN 6000000 AND 10000000;
SELECT name FROM employees WHERE name LIKE 'A%' OR name LIKE 'G%';
SELECT name FROM employees WHERE dept_id IS NULL;
SELECT name FROM employees WHERE dept_id IS NOT NULL AND active = 1 ORDER BY id;

-- ── [5] 집계 함수 (COUNT / SUM / AVG / MIN / MAX) ───────────────────────
SELECT
    COUNT(*) AS total,
    SUM(amount) AS payroll,
    AVG(amount) AS avg_sal,
    MAX(amount) AS max_sal,
    MIN(amount) AS min_sal
FROM salaries;

-- ── [6] GROUP BY / HAVING ────────────────────────────────────────────────
SELECT grade, COUNT(*) AS cnt, AVG(amount) AS avg_sal
    FROM salaries GROUP BY grade HAVING cnt >= 2 ORDER BY avg_sal DESC;

-- ── [7] JOIN (INNER / LEFT / RIGHT) ─────────────────────────────────────
-- INNER JOIN
SELECT e.name, d.name AS dept, s.amount, s.grade
    FROM employees e
    JOIN departments d ON e.dept_id = d.id
    JOIN salaries s ON e.id = s.employee_id
    ORDER BY s.amount DESC;
-- LEFT JOIN (Henry: dept NULL → 포함)
SELECT e.name, d.name AS dept
    FROM employees e LEFT JOIN departments d ON e.dept_id = d.id
    ORDER BY e.id;

-- ── [8] 서브쿼리 (IN / scalar / EXISTS / FROM절) ─────────────────────────
SELECT name FROM employees
    WHERE id IN (SELECT employee_id FROM salaries WHERE amount > 9000000);
SELECT employee_id, amount FROM salaries
    WHERE amount > (SELECT AVG(amount) FROM salaries);
SELECT name FROM employees
    WHERE EXISTS (SELECT 1 FROM salaries WHERE employee_id = employees.id AND amount > 11000000);
SELECT grade, avg_amt
    FROM (SELECT grade, AVG(amount) AS avg_amt FROM salaries GROUP BY grade) AS gs
    WHERE avg_amt > 7000000;

-- ── [9] UNION / UNION ALL ────────────────────────────────────────────────
SELECT name FROM employees WHERE dept_id = 1
UNION
SELECT name FROM employees WHERE dept_id = 3;

SELECT employee_id, amount FROM salaries WHERE grade = 'S5'
UNION ALL
SELECT employee_id, amount FROM salaries WHERE grade = 'S1'
ORDER BY amount DESC;

-- ── [10] 스칼라 함수 — 문자열 ────────────────────────────────────────────
SELECT
    UPPER(name)              AS upper_name,
    LOWER(name)              AS lower_name,
    LENGTH(name)             AS name_len,
    CONCAT(name, '@co.com')  AS email,
    TRIM('  hello  ')        AS trimmed,
    SUBSTR(name, 1, 3)       AS abbr,
    REPLACE(position, 'Engineer', 'Dev') AS new_pos,
    LPAD(id, 5, '0')         AS padded_id,
    RPAD(name, 12, '.')      AS padded_name
FROM employees WHERE active = 1 LIMIT 4;

-- ── [11] 스칼라 함수 — 수학 ──────────────────────────────────────────────
SELECT
    salary / 1000000              AS sal_M,
    ROUND(salary / 1000000, 2)    AS sal_M_rounded,
    ABS(-999)                     AS abs_val,
    CEIL(3.14)                    AS ceil_val,
    FLOOR(3.99)                   AS floor_val,
    MOD(salary, 1000000)          AS remainder
FROM employees WHERE id <= 3;

-- 함수 인자 내 산술식 (ArithExpr::Func)
SELECT ROUND(9500000 / 1000000, 1) AS nine_pt_five, ROUND(ABS(-3.7), 0) AS four;

-- ── [12] 스칼라 함수 — 날짜 ──────────────────────────────────────────────
SELECT
    name,
    hire_date,
    DATEDIFF('2026-05-03', hire_date)              AS days_worked,
    DATE_ADD(hire_date, INTERVAL 365 DAY)          AS one_yr_after,
    DATE_ADD(hire_date, INTERVAL 6 MONTH)          AS six_mo_after,
    DATE_FORMAT(hire_date, '%Y년 %m월 %d일')       AS formatted
FROM employees WHERE active = 1 ORDER BY hire_date LIMIT 4;

-- ── [13] NULL 처리 함수 / CAST / FROM 없는 스칼라 SELECT ─────────────────
SELECT COALESCE(dept_id, -1) AS safe_dept FROM employees WHERE dept_id IS NULL;
SELECT IFNULL(dept_id, 0)    AS dept      FROM employees WHERE id = 8;
SELECT NULLIF(dept_id, 4)    AS non_hr    FROM employees ORDER BY id LIMIT 5;

-- CAST
SELECT CAST('2026' AS INT) AS yr, CAST('3.14' AS FLOAT) AS pi;
SELECT CAST(salary AS FLOAT) AS sal_f FROM employees ORDER BY id LIMIT 3;

-- FROM 없는 스칼라 SELECT
SELECT 1 + 1 AS two, 10 * 3 AS thirty;
SELECT CAST('2026-05-03' AS DATE) AS today;

-- ── [14] CASE WHEN ───────────────────────────────────────────────────────
SELECT employee_id, amount,
    CASE
        WHEN amount >= 10000000 THEN 'Executive'
        WHEN amount >= 8000000  THEN 'Senior'
        WHEN amount >= 6000000  THEN 'Mid'
        ELSE 'Junior'
    END AS pay_level
FROM salaries ORDER BY amount DESC;

-- ── [15] CTE (WITH ... AS) ───────────────────────────────────────────────
WITH top_sal AS (
    SELECT employee_id, amount, grade FROM salaries WHERE amount > 8000000
),
dept_summary AS (
    SELECT dept_id, COUNT(*) AS cnt FROM employees GROUP BY dept_id
)
SELECT t.employee_id, t.amount, t.grade
FROM top_sal t ORDER BY t.amount DESC;

-- INSERT ... SELECT (CTE 활용)
CREATE TABLE sal_archive (id INT PRIMARY KEY, employee_id INT, amount INT, grade VARCHAR(5));
INSERT INTO sal_archive
    SELECT id, employee_id, amount, grade FROM salaries WHERE amount > 8000000;
SELECT * FROM sal_archive ORDER BY amount DESC;
TRUNCATE TABLE sal_archive;
DROP TABLE sal_archive;

-- ── [16] ALTER TABLE ─────────────────────────────────────────────────────
ALTER TABLE employees ADD COLUMN email VARCHAR(100);
ALTER TABLE employees MODIFY COLUMN email VARCHAR(150);
UPDATE employees SET email = 'user@hrdb.com' WHERE active = 1;
SELECT id, name, email FROM employees WHERE active = 1 ORDER BY id LIMIT 3;
ALTER TABLE employees RENAME COLUMN email TO contact;
ALTER TABLE employees DROP COLUMN contact;
DESCRIBE employees;

-- ── [17] UPDATE 산술식 / 다중 UPDATE ────────────────────────────────────
UPDATE employees SET position = 'Principal Engineer' WHERE id = 1;
UPDATE salaries SET amount = amount * 1.05 WHERE grade = 'S3';
SELECT e.name, s.grade, s.amount FROM employees e JOIN salaries s ON e.id = s.employee_id ORDER BY e.id;

-- ── [18] FK CASCADE DELETE ───────────────────────────────────────────────
-- Frank(id=6, HR) 삭제 → salaries(employee_id=6) 자동 CASCADE 삭제
DELETE FROM employees WHERE id = 6;
SELECT * FROM salaries WHERE employee_id = 6;  -- 0 rows

-- ── [19] EXPLAIN ─────────────────────────────────────────────────────────
EXPLAIN SELECT * FROM employees WHERE dept_id = 1;
EXPLAIN SELECT * FROM salaries WHERE employee_id = 1;
EXPLAIN SELECT * FROM employees WHERE active = 1;

-- ── [20] VIEW 조회 ───────────────────────────────────────────────────────
SELECT * FROM v_active_emp ORDER BY id;
SELECT * FROM v_high_earners ORDER BY amount DESC;
SELECT * FROM v_emp_detail ORDER BY amount DESC;

-- ── [21] TRANSACTION + SAVEPOINT ────────────────────────────────────────
BEGIN;
INSERT INTO employees (name, dept_id, position, hire_date, salary)
    VALUES ('Ivan', 1, 'Researcher', '2024-01-15', 5500000);
SAVEPOINT sp1;
UPDATE employees SET position = 'Senior Researcher' WHERE name = 'Ivan';
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT name, position, salary FROM employees WHERE name = 'Ivan';

BEGIN;
UPDATE salaries SET amount = 1 WHERE id = 1;
ROLLBACK;
SELECT amount FROM salaries WHERE id = 1;  -- 원래값 유지

-- ── [22] ISOLATION LEVEL ─────────────────────────────────────────────────
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;
SHOW ISOLATION LEVEL;

-- ── [23] CHECKPOINT / VACUUM / SHOW * ────────────────────────────────────
CHECKPOINT;
VACUUM;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;

-- ═══════════════════════════════════════════════════════════════════════════
-- DATABASE 2 : shopdb (전자상거래)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE DATABASE shopdb;
USE shopdb;

CREATE TABLE categories (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE,
    discount_rate INT DEFAULT 0
);
CREATE TABLE products (
    id          INT PRIMARY KEY AUTO INCREMENT,
    name        VARCHAR(100) NOT NULL,
    category_id INT,
    price       INT CHECK (price > 0),
    stock       INT DEFAULT 0,
    FOREIGN KEY (category_id) REFERENCES categories(id) ON DELETE SET NULL ON UPDATE CASCADE
);
CREATE TABLE orders (
    id         INT PRIMARY KEY AUTO INCREMENT,
    product_id INT,
    quantity   INT CHECK (quantity > 0),
    total      INT,
    status     ENUM('pending','shipped','done') DEFAULT 'pending',
    FOREIGN KEY (product_id) REFERENCES products(id) ON DELETE SET NULL
);

INSERT INTO categories (name, discount_rate) VALUES
    ('Electronics', 10), ('Clothing', 20), ('Food', 5);
INSERT INTO products (name, category_id, price, stock) VALUES
    ('Laptop',  1, 1200000, 15), ('Phone',  1, 800000, 30),
    ('T-Shirt', 2,   25000, 100), ('Jeans', 2,  60000, 50),
    ('Coffee',  3,   15000, 200), ('Bread', 3,   3500, 80);
INSERT INTO orders (product_id, quantity, total, status) VALUES
    (1, 2, 2400000, 'done'), (2, 5, 4000000, 'shipped'),
    (3, 10, 250000, 'done'), (4, 3, 180000, 'pending'),
    (1, 1, 1200000, 'done'), (5, 20, 300000, 'shipped');

CREATE INDEX idx_prod_cat   ON products (category_id);
CREATE INDEX idx_prod_price ON products (price);
CREATE INDEX idx_ord_prod   ON orders (product_id);

CREATE VIEW v_top_products AS
    SELECT p.id, p.name, p.price, c.name AS category
    FROM products p JOIN categories c ON p.category_id = c.id
    WHERE p.price > 50000;
CREATE VIEW v_order_summary AS
    SELECT o.id, p.name AS product, o.quantity, o.total, o.status
    FROM orders o JOIN products p ON o.product_id = p.id;

SHOW TABLES;

-- FK ON UPDATE CASCADE 검증: category id 변경 → products.category_id 연쇄 변경
SELECT id, name, category_id FROM products WHERE category_id = 1;
-- (Electronics: id=1)  — category id 직접 변경 불가(PK), 이름만 변경 테스트
UPDATE categories SET discount_rate = 15 WHERE id = 1;
SELECT * FROM categories;

-- INSERT IGNORE
INSERT INTO categories (name) VALUES ('Electronics');          -- 에러 (UNIQUE 위반)
INSERT IGNORE INTO categories (name) VALUES ('Electronics');   -- 무시
INSERT IGNORE INTO categories (name) VALUES ('Books'), ('Clothing'), ('Sports');
SELECT * FROM categories ORDER BY id;

-- ON DUPLICATE KEY UPDATE
INSERT INTO products (id, name, category_id, price, stock)
    VALUES (1, 'Laptop', 1, 1200000, 15)
    ON DUPLICATE KEY UPDATE stock = stock + 5;
SELECT id, name, stock FROM products WHERE id = 1;

-- 멀티 테이블 DELETE
DELETE orders, products
FROM orders
JOIN products ON orders.product_id = products.id
WHERE products.name = 'Bread';
SELECT * FROM orders ORDER BY id;
SELECT * FROM products ORDER BY id;

-- VIEW 조회 / EXPLAIN
SELECT * FROM v_top_products ORDER BY price DESC;
SELECT * FROM v_order_summary WHERE status != 'pending' ORDER BY total DESC;
EXPLAIN SELECT * FROM products WHERE category_id = 1;
EXPLAIN SELECT * FROM products WHERE price > 100000;

-- ═══════════════════════════════════════════════════════════════════════════
-- DATABASE 3 : logdb (시스템 로그)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE DATABASE logdb;
USE logdb;

CREATE TABLE servers (
    id        INT PRIMARY KEY AUTO INCREMENT,
    hostname  VARCHAR(50) NOT NULL UNIQUE,
    region    VARCHAR(20),
    cpu_cores INT DEFAULT 4
);
CREATE TABLE events (
    id          INT PRIMARY KEY AUTO INCREMENT,
    server_id   INT,
    severity    ENUM('INFO','WARN','ERROR'),
    message     VARCHAR(200),
    response_ms INT,
    FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);
CREATE TABLE metrics (
    id        INT PRIMARY KEY AUTO INCREMENT,
    server_id INT,
    cpu_pct   DOUBLE,
    mem_pct   DOUBLE,
    checkin   TIME,
    FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

INSERT INTO servers (hostname, region, cpu_cores) VALUES
    ('web-01', 'ap-seoul', 8), ('web-02', 'ap-seoul', 8),
    ('db-01',  'ap-busan', 16), ('cache-01', 'ap-seoul', 4);
INSERT INTO events (server_id, severity, message, response_ms) VALUES
    (1, 'INFO',  'Request processed',         45),
    (1, 'WARN',  'Memory usage high',         120),
    (2, 'INFO',  'Request processed',         38),
    (3, 'ERROR', 'Disk I/O timeout',          5000),
    (3, 'WARN',  'CPU spike detected',        200),
    (3, 'ERROR', 'Connection pool exhausted', 3000),
    (4, 'INFO',  'Cache hit',                 5),
    (4, 'WARN',  'Cache eviction',            80);
INSERT INTO metrics (server_id, cpu_pct, mem_pct, checkin) VALUES
    (1, 45.5, 62.3, '09:00:00'), (1, 78.2, 71.0, '10:00:00'),
    (2, 32.1, 55.8, '09:00:00'), (3, 95.7, 88.4, '09:00:00'),
    (3, 91.2, 90.1, '10:00:00'), (4, 12.5, 40.0, '09:00:00');

CREATE INDEX idx_ev_server   ON events (server_id);
CREATE INDEX idx_ev_severity ON events (severity);
CREATE INDEX idx_mt_server   ON metrics (server_id);

CREATE VIEW v_error_events AS
    SELECT server_id, message, response_ms FROM events WHERE severity = 'ERROR';
CREATE VIEW v_server_load AS
    SELECT server_id, AVG(cpu_pct) AS avg_cpu, MAX(cpu_pct) AS peak_cpu, AVG(mem_pct) AS avg_mem
    FROM metrics GROUP BY server_id;

SHOW TABLES;

-- JOIN + WHERE IN + ORDER BY
SELECT s.hostname, e.severity, e.message, e.response_ms
    FROM events e JOIN servers s ON e.server_id = s.id
    WHERE e.severity IN ('ERROR', 'WARN')
    ORDER BY e.response_ms DESC;

-- DOUBLE 타입 조건
SELECT * FROM metrics WHERE cpu_pct > 50.0 ORDER BY cpu_pct DESC;

-- 뷰 조회
SELECT * FROM v_error_events;
SELECT * FROM v_server_load ORDER BY avg_cpu DESC;
DESCRIBE metrics;

-- FK CASCADE DELETE: web-01 삭제 → events/metrics 연쇄 삭제
DELETE FROM servers WHERE hostname = 'web-01';
SELECT COUNT(*) AS ev_after  FROM events  WHERE server_id = 1;  -- 0
SELECT COUNT(*) AS met_after FROM metrics WHERE server_id = 1;  -- 0

-- ═══════════════════════════════════════════════════════════════════════════
-- DATABASE 4 : testdb (신기능 전용 테스트)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE DATABASE testdb;
USE testdb;

-- ── [24] GROUP_CONCAT ────────────────────────────────────────────────────
CREATE TABLE dept (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(30) NOT NULL UNIQUE
);
CREATE TABLE emp (
    id      INT PRIMARY KEY AUTO INCREMENT,
    name    VARCHAR(30) NOT NULL,
    dept_id INT,
    salary  INT
);
INSERT INTO dept (name) VALUES ('Engineering'), ('Marketing'), ('Finance');
INSERT INTO emp (name, dept_id, salary) VALUES
    ('Alice', 1, 9000000), ('Bob',   1, 8500000), ('Grace', 1, 9500000),
    ('Carol', 2, 7200000), ('Dave',  2, 5800000),
    ('Eve',   3, 12000000), ('Frank', 3, 6500000);

-- GROUP_CONCAT with SEPARATOR
SELECT d.name AS dept, GROUP_CONCAT(e.name SEPARATOR ', ') AS members
    FROM emp e JOIN dept d ON e.dept_id = d.id
    GROUP BY d.name ORDER BY d.name;

-- GROUP_CONCAT without SEPARATOR (comma default)
SELECT d.name AS dept, GROUP_CONCAT(e.name) AS members
    FROM emp e JOIN dept d ON e.dept_id = d.id
    GROUP BY d.name ORDER BY d.name;

-- GROUP_CONCAT + HAVING
SELECT dept_id, GROUP_CONCAT(name SEPARATOR ' / ') AS team
    FROM emp GROUP BY dept_id HAVING COUNT(*) >= 3;

-- ── [25] FK ON DELETE SET DEFAULT ───────────────────────────────────────
CREATE TABLE position_types (
    id   INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(30) NOT NULL
);
CREATE TABLE staff (
    id          INT PRIMARY KEY AUTO INCREMENT,
    name        VARCHAR(30) NOT NULL,
    position_id INT DEFAULT 0,
    FOREIGN KEY (position_id) REFERENCES position_types(id) ON DELETE SET DEFAULT
);
INSERT INTO position_types (name) VALUES ('Engineer'), ('Manager'), ('Intern');
INSERT INTO staff (name, position_id) VALUES
    ('Tom', 1), ('Jane', 1), ('Sam', 2), ('Lee', 3);

SELECT id, name, position_id FROM staff ORDER BY id;
DELETE FROM position_types WHERE id = 1;  -- Tom·Jane → position_id = 0 (DEFAULT)
SELECT id, name, position_id FROM staff ORDER BY id;

-- ── [26] WITH RECURSIVE — 계층 트리 ─────────────────────────────────────
CREATE TABLE org_tree (
    id        INT PRIMARY KEY AUTO INCREMENT,
    name      VARCHAR(50),
    parent_id INT
);
INSERT INTO org_tree (name, parent_id) VALUES
    ('CEO',           NULL),
    ('CTO',           1), ('CFO',          1),
    ('Backend Lead',  2), ('Frontend Lead', 2),
    ('Alice',         4), ('Bob',           4), ('Carol', 5);

WITH RECURSIVE hierarchy AS (
    SELECT id, name, parent_id, 0 AS depth
        FROM org_tree WHERE parent_id IS NULL
    UNION ALL
    SELECT o.id, o.name, o.parent_id, h.depth + 1
        FROM org_tree o JOIN hierarchy h ON o.parent_id = h.id
)
SELECT id, name, depth FROM hierarchy ORDER BY depth, id;

-- ── [27] WITH RECURSIVE — 숫자 시퀀스 1~10 ──────────────────────────────
CREATE TABLE seed (n INT PRIMARY KEY);
INSERT INTO seed VALUES (1);

WITH RECURSIVE seq AS (
    SELECT n FROM seed
    UNION ALL
    SELECT n + 1 FROM seq WHERE n < 10
)
SELECT n FROM seq ORDER BY n;

-- ── [28] INSERT IGNORE / ON DUPLICATE KEY UPDATE ─────────────────────────
INSERT IGNORE INTO dept (name) VALUES ('Engineering');           -- UNIQUE 위반 → 무시 (0 rows)
INSERT IGNORE INTO dept (name) VALUES ('Legal'), ('Marketing'), ('HR');  -- Marketing 중복 무시
SELECT * FROM dept ORDER BY id;

INSERT INTO emp (id, name, dept_id, salary)
    VALUES (1, 'Alice', 1, 10000000)
    ON DUPLICATE KEY UPDATE salary = 10000000;
SELECT id, name, salary FROM emp WHERE id = 1;  -- salary = 10000000

-- ── [29] UPDATE SET 스칼라 함수 (ArithExpr::Func) ───────────────────────
-- UPDATE 우변에서 CONCAT, UPPER 등 직접 사용
UPDATE emp SET name = CONCAT(name, '_', dept_id) WHERE id = 3;
SELECT id, name FROM emp WHERE id = 3;  -- e.g. Carol_2
UPDATE emp SET name = UPPER(name) WHERE id = 2;
SELECT id, name FROM emp WHERE id = 2;  -- BOB or similar

-- ── [30] NULLIF / LPAD / RPAD / CAST / DATEDIFF / DATE_ADD ─────────────
-- 직원 날짜 데이터 (임시 테이블)
CREATE TABLE emp_dates (
    id        INT PRIMARY KEY AUTO INCREMENT,
    name      VARCHAR(30),
    hire_date DATE
);
INSERT INTO emp_dates (name, hire_date) VALUES
    ('Alice', '2019-03-15'), ('Bob', '2020-07-01'),
    ('Carol', '2018-11-20'), ('Eve', '2015-01-05');

SELECT id, LPAD(id, 5, '0') AS padded_id, RPAD(name, 10, '.') AS padded FROM emp_dates;
SELECT NULLIF(id, 3) AS non_carol FROM emp_dates ORDER BY id;
SELECT name, hire_date, DATEDIFF('2026-05-03', hire_date) AS days FROM emp_dates ORDER BY days DESC;
SELECT name, DATE_ADD(hire_date, INTERVAL 365 DAY) AS one_yr  FROM emp_dates LIMIT 3;
SELECT name, DATE_ADD(hire_date, INTERVAL 6 MONTH) AS six_mo  FROM emp_dates LIMIT 3;
SELECT CAST('123' AS INT) AS n, CAST('3.14' AS FLOAT) AS f;
SELECT CAST(hire_date AS TEXT) AS dt_str FROM emp_dates LIMIT 2;

-- ── [31] IF() 함수 ───────────────────────────────────────────────────────
SELECT name, salary, IF(salary > 8000000, 'High', 'Normal') AS tier
    FROM emp ORDER BY salary DESC;

-- ── [32] 복합 인덱스 / EXPLAIN ───────────────────────────────────────────
CREATE INDEX idx_emp_dept_sal ON emp (dept_id, salary);
EXPLAIN SELECT * FROM emp WHERE dept_id = 1;

-- ═══════════════════════════════════════════════════════════════════════════
-- [33] 사용자 관리 (CREATE USER / GRANT / REVOKE / SHOW GRANTS / DROP USER)
-- ═══════════════════════════════════════════════════════════════════════════
CREATE USER 'testuser'@'localhost' IDENTIFIED BY 'pass1234';
CREATE USER 'readonly'@'%';
CREATE USER IF NOT EXISTS 'testuser'@'localhost';  -- 중복 스킵

GRANT SELECT, INSERT, UPDATE ON testdb.emp  TO 'testuser'@'localhost';
GRANT SELECT                  ON testdb.dept TO 'testuser'@'localhost';
GRANT ALL PRIVILEGES ON *.* TO 'readonly'@'%' WITH GRANT OPTION;

SHOW GRANTS FOR 'testuser'@'localhost';
SHOW GRANTS;

REVOKE INSERT, UPDATE ON testdb.emp FROM 'testuser'@'localhost';
SHOW GRANTS FOR 'testuser'@'localhost';

DROP USER 'readonly'@'%';
DROP USER IF EXISTS 'nobody'@'localhost';  -- 없어도 에러 없음
SHOW GRANTS;

-- ═══════════════════════════════════════════════════════════════════════════
-- [34] SHOW DATABASES / SHOW TABLES / DESCRIBE
-- ═══════════════════════════════════════════════════════════════════════════
SHOW DATABASES;
USE testdb;
SHOW TABLES;
DESCRIBE emp;

-- ═══════════════════════════════════════════════════════════════════════════
-- CLEANUP
-- ═══════════════════════════════════════════════════════════════════════════
USE testdb;
DROP TABLE IF EXISTS emp_dates;
DROP TABLE IF EXISTS seed;
DROP TABLE IF EXISTS org_tree;
DROP TABLE IF EXISTS staff;
DROP TABLE IF EXISTS position_types;
DROP TABLE IF EXISTS emp;
DROP TABLE IF EXISTS dept;

USE shopdb;
DROP INDEX IF EXISTS idx_prod_cat;
DROP INDEX IF EXISTS idx_prod_price;
DROP INDEX IF EXISTS idx_ord_prod;
DROP VIEW  IF EXISTS v_top_products;
DROP VIEW  IF EXISTS v_order_summary;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS categories;

USE hrdb;
DROP INDEX IF EXISTS idx_emp_dept;
DROP INDEX IF EXISTS idx_emp_active;
DROP INDEX IF EXISTS idx_sal_emp;
DROP VIEW  IF EXISTS v_active_emp;
DROP VIEW  IF EXISTS v_high_earners;
DROP VIEW  IF EXISTS v_emp_detail;
DROP TABLE IF EXISTS salaries;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS departments;

USE logdb;
DROP INDEX IF EXISTS idx_ev_server;
DROP INDEX IF EXISTS idx_ev_severity;
DROP INDEX IF EXISTS idx_mt_server;
DROP VIEW  IF EXISTS v_error_events;
DROP VIEW  IF EXISTS v_server_load;
DROP TABLE IF EXISTS metrics;
DROP TABLE IF EXISTS events;
DROP TABLE IF EXISTS servers;

DROP USER IF EXISTS 'testuser'@'localhost';

DROP DATABASE testdb;
DROP DATABASE shopdb;
DROP DATABASE hrdb;
DROP DATABASE logdb;

SHOW DATABASES;
