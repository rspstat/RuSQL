-- SETUP
DROP DATABASE IF EXISTS shopdb;
DROP DATABASE IF EXISTS hrdb;
DROP DATABASE IF EXISTS logdb;

-- DATABASE 1: shopdb (전자상거래)
CREATE DATABASE shopdb;
USE shopdb;

-- Tables: 3개
CREATE TABLE categories (
    id INT PRIMARY KEY AUTO INCREMENT,
    name VARCHAR(50) NOT NULL UNIQUE,
    discount_rate INT DEFAULT 0
);
CREATE TABLE products (
    id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(100) NOT NULL,
    category_id INT, price INT CHECK (price > 0), stock INT DEFAULT 0,
    FOREIGN KEY (category_id) REFERENCES categories(id) ON DELETE SET NULL ON UPDATE CASCADE
);
CREATE TABLE orders (
    id INT PRIMARY KEY AUTO INCREMENT, product_id INT,
    quantity INT CHECK (quantity > 0), total INT,
    status ENUM('pending','shipped','done') DEFAULT 'pending',
    FOREIGN KEY (product_id) REFERENCES products(id) ON DELETE SET NULL
);

INSERT INTO categories (name, discount_rate) VALUES ('Electronics', 10), ('Clothing', 20), ('Food', 5);
INSERT INTO products (name, category_id, price, stock) VALUES
    ('Laptop', 1, 1200000, 15), ('Phone', 1, 800000, 30),
    ('T-Shirt', 2, 25000, 100), ('Jeans', 2, 60000, 50),
    ('Coffee', 3, 15000, 200), ('Bread', 3, 3500, 80);
INSERT INTO orders (product_id, quantity, total, status) VALUES
    (1, 2, 2400000, 'done'), (2, 5, 4000000, 'shipped'),
    (3, 10, 250000, 'done'), (4, 3, 180000, 'pending'),
    (1, 1, 1200000, 'done'), (5, 20, 300000, 'shipped');

-- Indexes: 3개
CREATE INDEX idx_products_category ON products (category_id);
CREATE INDEX idx_orders_product ON orders (product_id);
CREATE INDEX idx_products_price ON products (price);

-- Views: 2개
CREATE VIEW v_top_products AS
    SELECT p.id, p.name, p.price, c.name AS category
    FROM products p JOIN categories c ON p.category_id = c.id WHERE p.price > 50000;
CREATE VIEW v_order_summary AS
    SELECT o.id, p.name AS product, o.quantity, o.total, o.status
    FROM orders o JOIN products p ON o.product_id = p.id;

SHOW TABLES;

-- DATABASE 2: hrdb (인사관리)
CREATE DATABASE hrdb;
USE hrdb;

-- Tables: 2개
CREATE TABLE employees (
    id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(50) NOT NULL,
    dept VARCHAR(30), position VARCHAR(30), hire_year YEAR, active INT DEFAULT 1
);
CREATE TABLE salaries (
    id INT PRIMARY KEY AUTO INCREMENT, employee_id INT,
    amount INT CHECK (amount > 0), grade ENUM('S1','S2','S3','S4','S5'),
    FOREIGN KEY (employee_id) REFERENCES employees(id) ON DELETE CASCADE
);

INSERT INTO employees (name, dept, position, hire_year, active) VALUES
    ('Alice', 'Engineering', 'Lead Engineer', 2019, 1),
    ('Bob', 'Engineering', 'Senior Engineer', 2020, 1),
    ('Carol', 'Marketing', 'Marketing Manager', 2018, 1),
    ('Dave', 'Marketing', 'Marketing Analyst', 2021, 1),
    ('Eve', 'HR', 'HR Manager', 2017, 1),
    ('Frank', 'HR', 'HR Specialist', 2022, 0),
    ('Grace', 'Finance', 'CFO', 2015, 1),
    ('Henry', NULL, 'Consultant', 2023, 1);
INSERT INTO salaries (employee_id, amount, grade) VALUES
    (1, 9500000, 'S4'), (2, 8500000, 'S3'), (3, 7200000, 'S3'), (4, 5800000, 'S2'),
    (5, 6500000, 'S2'), (6, 4500000, 'S1'), (7, 12000000, 'S5'), (8, 5000000, 'S1');

-- Indexes: 3개
CREATE INDEX idx_emp_dept ON employees (dept);
CREATE INDEX idx_emp_active ON employees (active);
CREATE INDEX idx_sal_employee ON salaries (employee_id);

-- Views: 3개
CREATE VIEW v_active_employees AS
    SELECT id, name, dept, position FROM employees WHERE active = 1;
CREATE VIEW v_high_earners AS
    SELECT employee_id, amount, grade FROM salaries WHERE amount > 7000000;
CREATE VIEW v_emp_detail AS
    SELECT e.id, e.name, e.position, e.dept, s.amount, s.grade
    FROM employees e LEFT JOIN salaries s ON e.id = s.employee_id;

SHOW TABLES;

-- DATABASE 3: logdb (시스템 로그)
CREATE DATABASE logdb;
USE logdb;

-- Tables: 3개
CREATE TABLE servers (
    id INT PRIMARY KEY AUTO INCREMENT, hostname VARCHAR(50) NOT NULL UNIQUE,
    region VARCHAR(20), cpu_cores INT DEFAULT 4
);
CREATE TABLE events (
    id INT PRIMARY KEY AUTO INCREMENT, server_id INT,
    severity ENUM('INFO','WARN','ERROR'), message VARCHAR(200), response_ms INT,
    FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);
CREATE TABLE metrics (
    id INT PRIMARY KEY AUTO INCREMENT, server_id INT,
    cpu_pct DOUBLE, mem_pct DOUBLE, checkin TIME,
    FOREIGN KEY (server_id) REFERENCES servers(id) ON DELETE CASCADE
);

INSERT INTO servers (hostname, region, cpu_cores) VALUES
    ('web-01', 'ap-seoul', 8), ('web-02', 'ap-seoul', 8),
    ('db-01', 'ap-busan', 16), ('cache-01', 'ap-seoul', 4);
INSERT INTO events (server_id, severity, message, response_ms) VALUES
    (1, 'INFO', 'Request processed', 45), (1, 'WARN', 'Memory usage high', 120),
    (2, 'INFO', 'Request processed', 38), (3, 'ERROR', 'Disk I/O timeout', 5000),
    (3, 'WARN', 'CPU spike detected', 200), (3, 'ERROR', 'Connection pool exhausted', 3000),
    (4, 'INFO', 'Cache hit', 5), (4, 'WARN', 'Cache eviction', 80);
INSERT INTO metrics (server_id, cpu_pct, mem_pct, checkin) VALUES
    (1, 45.5, 62.3, '09:00:00'), (1, 78.2, 71.0, '10:00:00'),
    (2, 32.1, 55.8, '09:00:00'), (3, 95.7, 88.4, '09:00:00'),
    (3, 91.2, 90.1, '10:00:00'), (4, 12.5, 40.0, '09:00:00');

-- Indexes: 3개
CREATE INDEX idx_events_server ON events (server_id);
CREATE INDEX idx_events_severity ON events (severity);
CREATE INDEX idx_metrics_server ON metrics (server_id);

-- Views: 2개
CREATE VIEW v_error_events AS
    SELECT server_id, message, response_ms FROM events WHERE severity = 'ERROR';
CREATE VIEW v_server_load AS
    SELECT server_id, AVG(cpu_pct) AS avg_cpu, MAX(cpu_pct) AS peak_cpu, AVG(mem_pct) AS avg_mem
    FROM metrics GROUP BY server_id;

SHOW TABLES;

-- SELECT / ORDER BY / LIMIT / OFFSET / DISTINCT / ARITHMETIC
USE hrdb;
SELECT id, name, position FROM employees WHERE active = 1 ORDER BY id;
SELECT id, name FROM employees ORDER BY id LIMIT 3 OFFSET 2;
SELECT DISTINCT dept FROM employees ORDER BY dept;
SELECT employee_id, amount, amount * 1.1 AS raise_10pct FROM salaries WHERE amount > 8000000;

-- IN / NOT IN / NOT / BETWEEN / LIKE / IS NULL
SELECT name FROM employees WHERE dept IN ('Engineering', 'Marketing');
SELECT name FROM employees WHERE dept NOT IN ('HR', 'Finance');
SELECT name FROM employees WHERE NOT (active = 1);
SELECT employee_id, amount FROM salaries WHERE amount BETWEEN 6000000 AND 9000000;
SELECT name FROM employees WHERE name LIKE 'A%' OR name LIKE 'G%';
SELECT name FROM employees WHERE dept IS NULL;
SELECT name FROM employees WHERE dept IS NOT NULL AND active = 1 ORDER BY id;

-- AGGREGATE / GROUP BY / HAVING
SELECT COUNT(*) AS total, AVG(amount) AS avg_sal, MAX(amount) AS max_sal, MIN(amount) AS min_sal, SUM(amount) AS payroll FROM salaries;
SELECT grade, COUNT(*) AS cnt, AVG(amount) AS avg_sal FROM salaries GROUP BY grade HAVING cnt >= 2 ORDER BY avg_sal DESC;

-- JOIN (INNER / LEFT)
SELECT e.name, e.dept, s.amount, s.grade
    FROM employees e JOIN salaries s ON e.id = s.employee_id
    ORDER BY s.amount DESC;
SELECT e.name, e.dept FROM employees e LEFT JOIN salaries s ON e.id = s.employee_id ORDER BY e.id;

-- SUBQUERY (IN / scalar / EXISTS / derived)
SELECT name FROM employees WHERE id IN (SELECT employee_id FROM salaries WHERE amount > 8000000);
SELECT employee_id, amount FROM salaries WHERE amount > (SELECT AVG(amount) FROM salaries);
SELECT name FROM employees WHERE EXISTS (SELECT 1 FROM salaries WHERE employee_id = employees.id AND amount > 9000000);
SELECT grade, avg_amt FROM (SELECT grade, AVG(amount) AS avg_amt FROM salaries GROUP BY grade) AS gs WHERE avg_amt > 6000000;

-- UNION / UNION ALL
SELECT name FROM employees WHERE dept = 'Engineering'
UNION SELECT name FROM employees WHERE dept = 'Finance';
SELECT employee_id, amount FROM salaries WHERE grade = 'S5'
UNION ALL SELECT employee_id, amount FROM salaries WHERE grade = 'S1'
ORDER BY amount DESC;

-- SCALAR FUNCTIONS / CASE WHEN
SELECT UPPER(name) AS up, LENGTH(name) AS len, CONCAT(name, '@company.com') AS email FROM employees WHERE active = 1 LIMIT 4;
SELECT COALESCE(dept, 'N/A') AS dept FROM employees WHERE dept IS NULL;
SELECT name, amount,
    CASE WHEN amount >= 10000000 THEN 'Executive'
         WHEN amount >= 7000000 THEN 'Senior'
         WHEN amount >= 5000000 THEN 'Mid'
         ELSE 'Junior' END AS pay_level
    FROM salaries ORDER BY amount DESC;

-- CTE
WITH high_sal AS (SELECT employee_id, amount, grade FROM salaries WHERE amount > 7000000)
SELECT * FROM high_sal ORDER BY amount DESC;

-- VIEW 조회
SELECT * FROM v_active_employees ORDER BY id;
SELECT * FROM v_high_earners ORDER BY amount DESC;
SELECT * FROM v_emp_detail ORDER BY amount DESC;

-- EXPLAIN (인덱스 활용 확인)
EXPLAIN SELECT * FROM employees WHERE dept = 'Engineering';
EXPLAIN SELECT * FROM salaries WHERE employee_id = 1;
EXPLAIN SELECT * FROM employees WHERE active = 1;

-- ALTER TABLE
ALTER TABLE employees ADD COLUMN email VARCHAR(100);
ALTER TABLE employees MODIFY COLUMN email VARCHAR(150);
ALTER TABLE employees RENAME COLUMN email TO contact;
ALTER TABLE employees DROP COLUMN contact;
DESCRIBE employees;

-- UPDATE / DELETE + FK CASCADE
UPDATE employees SET position = 'Principal Engineer' WHERE id = 1;
UPDATE salaries SET amount = amount * 1.05 WHERE grade = 'S3';
DELETE FROM employees WHERE id = 6;
SELECT e.name, s.amount FROM employees e JOIN salaries s ON e.id = s.employee_id ORDER BY e.id;

-- CONSTRAINT ERROR (expected ERROR)
INSERT INTO salaries (employee_id, amount, grade) VALUES (1, -500, 'S1');

-- INSERT ... SELECT / TRUNCATE
CREATE TABLE sal_archive (id INT PRIMARY KEY, employee_id INT, amount INT);
INSERT INTO sal_archive SELECT id, employee_id, amount FROM salaries WHERE amount > 8000000;
SELECT * FROM sal_archive ORDER BY amount DESC;
TRUNCATE TABLE sal_archive;
DROP TABLE sal_archive;

-- TRANSACTION + SAVEPOINT
BEGIN;
INSERT INTO employees (name, dept, position, hire_year) VALUES ('Ivan', 'Research', 'Researcher', 2024);
SAVEPOINT sp1;
UPDATE employees SET position = 'Senior Researcher' WHERE name = 'Ivan';
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT name, position FROM employees WHERE name = 'Ivan';

BEGIN;
UPDATE salaries SET amount = 1 WHERE id = 1;
ROLLBACK;
SELECT amount FROM salaries WHERE id = 1;

-- ISOLATION LEVEL
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- logdb: 특수 타입 / 뷰 조회
USE logdb;
SELECT s.hostname, e.severity, e.message, e.response_ms
    FROM events e JOIN servers s ON e.server_id = s.id
    WHERE e.severity IN ('ERROR', 'WARN') ORDER BY e.response_ms DESC;
SELECT * FROM metrics WHERE cpu_pct > 50.0 ORDER BY cpu_pct DESC;
SELECT * FROM v_error_events;
SELECT * FROM v_server_load ORDER BY avg_cpu DESC;
DESCRIBE metrics;

-- shopdb: 뷰 / 인덱스 확인
USE shopdb;
SELECT * FROM v_top_products ORDER BY price DESC;
SELECT * FROM v_order_summary WHERE status != 'pending' ORDER BY total DESC;
EXPLAIN SELECT * FROM products WHERE category_id = 1;
EXPLAIN SELECT * FROM orders WHERE product_id = 1;

-- ADMIN
USE hrdb;
SHOW TABLES;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
CHECKPOINT;
VACUUM;

-- CLEANUP
USE shopdb;
DROP INDEX IF EXISTS idx_products_category;
DROP INDEX IF EXISTS idx_orders_product;
DROP INDEX IF EXISTS idx_products_price;
DROP VIEW IF EXISTS v_top_products;
DROP VIEW IF EXISTS v_order_summary;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS categories;

USE hrdb;
DROP INDEX IF EXISTS idx_emp_dept;
DROP INDEX IF EXISTS idx_emp_active;
DROP INDEX IF EXISTS idx_sal_employee;
DROP VIEW IF EXISTS v_active_employees;
DROP VIEW IF EXISTS v_high_earners;
DROP VIEW IF EXISTS v_emp_detail;
DROP TABLE IF EXISTS salaries;
DROP TABLE IF EXISTS employees;

USE logdb;
DROP INDEX IF EXISTS idx_events_server;
DROP INDEX IF EXISTS idx_events_severity;
DROP INDEX IF EXISTS idx_metrics_server;
DROP VIEW IF EXISTS v_error_events;
DROP VIEW IF EXISTS v_server_load;
DROP TABLE IF EXISTS metrics;
DROP TABLE IF EXISTS events;
DROP TABLE IF EXISTS servers;

DROP DATABASE shopdb;
DROP DATABASE hrdb;
DROP DATABASE logdb;
