-- ================================================================ SETUP
DROP TABLE IF EXISTS order_items;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS departments;
DROP TABLE IF EXISTS employees;
DROP VIEW IF EXISTS active_employees;
DROP INDEX IF EXISTS idx_emp_dept;
-- ================================================================ CREATE TABLE
CREATE TABLE departments (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(50) NOT NULL UNIQUE, budget DECIMAL(12,2) DEFAULT 0.00);
CREATE TABLE employees (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(100) NOT NULL, dept_id INT, salary DECIMAL(10,2) CHECK (salary > 0), hire_date DATE, active BOOLEAN DEFAULT true, FOREIGN KEY (dept_id) REFERENCES departments(id) ON DELETE SET NULL ON UPDATE CASCADE);
CREATE TABLE products (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(100) NOT NULL, price DECIMAL(10,2) CHECK (price >= 0), stock INT DEFAULT 0);
CREATE TABLE orders (id INT PRIMARY KEY AUTO INCREMENT, emp_id INT, total DECIMAL(12,2), order_date DATE, FOREIGN KEY (emp_id) REFERENCES employees(id) ON DELETE RESTRICT);
CREATE TABLE order_items (order_id INT, product_id INT, qty INT NOT NULL, PRIMARY KEY (order_id, product_id));
-- ================================================================ INSERT
INSERT INTO departments (name, budget) VALUES ('Engineering', 500000.00);
INSERT INTO departments (name, budget) VALUES ('Marketing', 200000.00);
INSERT INTO departments (name, budget) VALUES ('HR', 100000.00);
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES ('Alice', 1, 95000.00, '2020-03-15', true);
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES ('Bob', 1, 85000.00, '2021-06-01', true);
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES ('Carol', 2, 72000.00, '2019-11-20', false);
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES ('Dave', 2, 68000.00, '2022-01-10', true);
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES ('Eve', 3, 60000.00, '2023-05-01', true);
INSERT INTO products (name, price, stock) VALUES ('Laptop', 1200.00, 50), ('Mouse', 25.00, 200), ('Keyboard', 75.00, 150), ('Monitor', 350.00, 80), ('Headset', 90.00, 120);
INSERT INTO orders (emp_id, total, order_date) VALUES (1, 1275.00, '2024-01-10');
INSERT INTO orders (emp_id, total, order_date) VALUES (2, 425.00, '2024-01-15');
INSERT INTO orders (emp_id, total, order_date) VALUES (1, 440.00, '2024-02-05');
INSERT INTO order_items VALUES (1, 1, 1), (1, 2, 3), (2, 3, 2), (2, 4, 1), (3, 2, 2), (3, 5, 2);
-- ================================================================ SELECT basic
SELECT * FROM departments;
SELECT id, name, salary FROM employees;
SELECT DISTINCT dept_id FROM employees;
SELECT id, name, salary FROM employees WHERE salary > 70000;
SELECT id, name, salary FROM employees WHERE dept_id = 1 AND salary > 80000;
SELECT id, name, salary FROM employees WHERE salary BETWEEN 65000 AND 90000;
SELECT id, name FROM employees WHERE name LIKE 'A%';
SELECT id, name FROM employees WHERE hire_date IS NOT NULL;
SELECT id, name FROM employees WHERE dept_id IS NULL;
-- ================================================================ ORDER BY / LIMIT
SELECT id, name, salary FROM employees ORDER BY salary DESC;
SELECT id, name, salary FROM employees ORDER BY dept_id ASC, salary DESC;
SELECT id, name, salary FROM employees ORDER BY salary DESC LIMIT 3;
-- ================================================================ AGGREGATE + GROUP BY + HAVING
SELECT COUNT(*) AS total_employees FROM employees;
SELECT dept_id, COUNT(*) AS cnt, AVG(salary) AS avg_sal, MAX(salary) AS max_sal FROM employees GROUP BY dept_id;
SELECT dept_id, COUNT(*) AS cnt FROM employees GROUP BY dept_id HAVING cnt > 1;
SELECT SUM(total) AS total_revenue FROM orders;
-- ================================================================ JOIN (table alias)
SELECT e.id, e.name, d.name AS dept FROM employees e JOIN departments d ON e.dept_id = d.id;
SELECT e.id, e.name, d.name AS dept FROM employees e LEFT JOIN departments d ON e.dept_id = d.id;
SELECT e.id, e.name, e.salary, d.name AS dept FROM employees e JOIN departments d ON e.dept_id = d.id WHERE e.salary > 70000 ORDER BY e.salary DESC;
SELECT o.id, e.name, o.total, o.order_date FROM orders o JOIN employees e ON o.emp_id = e.id;
-- ================================================================ SUBQUERY
SELECT id, name FROM employees WHERE dept_id IN (SELECT id FROM departments WHERE budget > 300000);
SELECT id, name FROM employees WHERE dept_id NOT IN (SELECT id FROM departments WHERE name = 'HR');
SELECT id, name FROM employees WHERE salary > (SELECT AVG(salary) FROM employees);
SELECT id, name FROM departments WHERE EXISTS (SELECT 1 FROM employees WHERE dept_id = departments.id AND salary > 90000);
-- ================================================================ FROM subquery
SELECT dept_id, avg_sal FROM (SELECT dept_id, AVG(salary) AS avg_sal FROM employees GROUP BY dept_id) AS dept_stats WHERE avg_sal > 75000;
-- ================================================================ STRING FUNCTIONS
SELECT id, UPPER(name) AS up, LOWER(name) AS lo, LENGTH(name) AS len FROM employees;
SELECT id, CONCAT(name, ' (', hire_date, ')') AS info FROM employees;
SELECT id, SUBSTR(name, 1, 3) AS short_name FROM employees;
SELECT id, REPLACE(name, 'Alice', 'Alicia') AS renamed FROM employees;
SELECT id, TRIM(name) AS trimmed FROM employees;
SELECT id, COALESCE(dept_id, 0) AS dept FROM employees;
SELECT id, IFNULL(dept_id, 0) AS dept FROM employees;
-- ================================================================ DATE FUNCTIONS
SELECT id, name, DATE_FORMAT(hire_date, '%Y/%m/%d') AS fmt_date FROM employees;
SELECT CURDATE() AS today FROM departments LIMIT 1;
SELECT NOW() AS now FROM departments LIMIT 1;
-- ================================================================ COMPOSITE PK
SELECT * FROM order_items;
SELECT order_id, SUM(qty) AS total_qty FROM order_items GROUP BY order_id;
-- ================================================================ CHECK VIOLATION (expected ERROR)
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES ('Hacker', 1, -1000.00, '2024-01-01', true);
INSERT INTO products (name, price, stock) VALUES ('Free', -1.00, 10);
-- ================================================================ FK VIOLATION (expected ERROR)
INSERT INTO orders (emp_id, total, order_date) VALUES (999, 100.00, '2024-01-01');
-- ================================================================ UPDATE + FK CASCADE
UPDATE departments SET name = 'Engineering Team' WHERE id = 1;
SELECT id, name, dept_id FROM employees WHERE dept_id = 1;
UPDATE employees SET salary = 100000.00 WHERE id = 1;
SELECT id, name, salary FROM employees WHERE id = 1;
-- ================================================================ INDEX + EXPLAIN
CREATE INDEX idx_emp_dept ON employees (dept_id);
EXPLAIN SELECT * FROM employees WHERE dept_id = 1;
EXPLAIN SELECT * FROM employees WHERE id = 2;
EXPLAIN SELECT * FROM employees WHERE id BETWEEN 1 AND 3;
EXPLAIN SELECT * FROM employees WHERE salary > 70000;
-- ================================================================ VIEW
CREATE VIEW active_employees AS SELECT id, name, dept_id, salary FROM employees WHERE active = true;
SELECT * FROM active_employees;
SELECT id, name FROM active_employees WHERE salary > 70000;
-- ================================================================ ALTER TABLE
ALTER TABLE products ADD COLUMN description TEXT;
ALTER TABLE products MODIFY COLUMN stock INT NOT NULL;
ALTER TABLE products RENAME COLUMN description TO notes;
DESCRIBE products;
ALTER TABLE products DROP COLUMN notes;
DESCRIBE products;
-- ================================================================ TRANSACTION + SAVEPOINT
BEGIN;
INSERT INTO products (name, price, stock) VALUES ('Tablet', 500.00, 30);
SAVEPOINT sp1;
INSERT INTO products (name, price, stock) VALUES ('Smartwatch', 300.00, 40);
ROLLBACK TO SAVEPOINT sp1;
SELECT id, name, price FROM products;
COMMIT;
SELECT id, name, price FROM products;
-- ================================================================ TRANSACTION ROLLBACK
BEGIN;
UPDATE employees SET salary = 200000.00 WHERE id = 2;
SELECT id, name, salary FROM employees WHERE id = 2;
ROLLBACK;
SELECT id, name, salary FROM employees WHERE id = 2;
-- ================================================================ ISOLATION LEVEL
SET ISOLATION LEVEL READ COMMITTED;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL REPEATABLE READ;
SHOW ISOLATION LEVEL;
-- ================================================================ DELETE + FK RESTRICT
DELETE FROM orders WHERE id = 1;
SELECT * FROM orders;
-- ================================================================ DISTINCT + LIKE + IS NULL
SELECT DISTINCT dept_id FROM employees ORDER BY dept_id;
SELECT id, name FROM employees WHERE name LIKE '%e%';
INSERT INTO employees (name, dept_id, salary, hire_date) VALUES ('Frank', NULL, 55000.00, '2024-03-01');
SELECT id, name FROM employees WHERE dept_id IS NULL;
-- ================================================================ VACUUM + SHOW
SHOW TABLES;
DESCRIBE employees;
SHOW BUFFER POOL;
CHECKPOINT;
VACUUM;
-- ================================================================ TRUNCATE + DROP
TRUNCATE TABLE order_items;
SELECT COUNT(*) AS cnt FROM order_items;
DROP TABLE order_items;
DROP TABLE orders;
DROP TABLE products;
DROP TABLE employees;
DROP TABLE departments;
SHOW TABLES;
