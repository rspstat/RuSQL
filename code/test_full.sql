-- SETUP
DROP TABLE IF EXISTS order_items;
DROP TABLE IF EXISTS orders;
DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS employees;
DROP TABLE IF EXISTS departments;
DROP VIEW IF EXISTS v_active;
DROP INDEX IF EXISTS idx_emp_dept;

-- CREATE TABLE
CREATE TABLE departments (
    id     INT PRIMARY KEY AUTO INCREMENT,
    name   VARCHAR(50) NOT NULL UNIQUE,
    budget DECIMAL(12,2) DEFAULT 0.00
);
CREATE TABLE employees (
    id        INT PRIMARY KEY AUTO INCREMENT,
    name      VARCHAR(100) NOT NULL,
    dept_id   INT,
    salary    DECIMAL(10,2) CHECK (salary > 0),
    hire_date DATE,
    active    BOOLEAN DEFAULT true,
    FOREIGN KEY (dept_id) REFERENCES departments(id) ON DELETE SET NULL ON UPDATE CASCADE
);
CREATE TABLE products (
    id    INT PRIMARY KEY AUTO INCREMENT,
    name  VARCHAR(100) NOT NULL,
    price DECIMAL(10,2) CHECK (price >= 0),
    stock INT DEFAULT 0
);
CREATE TABLE orders (
    id         INT PRIMARY KEY AUTO INCREMENT,
    emp_id     INT,
    total      DECIMAL(12,2),
    order_date DATE,
    FOREIGN KEY (emp_id) REFERENCES employees(id) ON DELETE RESTRICT
);
CREATE TABLE order_items (
    order_id   INT,
    product_id INT,
    qty        INT NOT NULL,
    PRIMARY KEY (order_id, product_id)
);

-- INSERT
INSERT INTO departments (name, budget) VALUES ('Engineering', 500000.00), ('Marketing', 200000.00), ('HR', 100000.00);
INSERT INTO employees (name, dept_id, salary, hire_date, active) VALUES
    ('Alice', 1, 95000.00, '2020-03-15', true),
    ('Bob',   1, 85000.00, '2021-06-01', true),
    ('Carol', 2, 72000.00, '2019-11-20', false),
    ('Dave',  2, 68000.00, '2022-01-10', true),
    ('Eve',   3, 60000.00, '2023-05-01', true);
INSERT INTO products (name, price, stock) VALUES ('Laptop', 1200.00, 50), ('Mouse', 25.00, 200), ('Keyboard', 75.00, 150);
INSERT INTO orders (emp_id, total, order_date) VALUES (1, 1275.00, '2024-01-10'), (2, 425.00, '2024-01-15');
INSERT INTO order_items VALUES (1, 1, 1), (1, 2, 3), (2, 3, 2);

-- SELECT / WHERE / ORDER BY / LIMIT
SELECT * FROM departments;
SELECT id, name, salary FROM employees WHERE salary > 70000 ORDER BY salary DESC LIMIT 3;
SELECT DISTINCT dept_id FROM employees ORDER BY dept_id;
SELECT id, name FROM employees WHERE salary BETWEEN 65000 AND 90000;
SELECT id, name FROM employees WHERE name LIKE 'A%' OR name LIKE 'B%';
SELECT id, name FROM employees WHERE hire_date IS NOT NULL;

-- AGGREGATE + GROUP BY + HAVING
SELECT COUNT(*) AS total FROM employees;
SELECT dept_id, COUNT(*) AS cnt, AVG(salary) AS avg_sal, MAX(salary) AS max_sal FROM employees GROUP BY dept_id HAVING cnt > 1;
SELECT SUM(total) AS revenue FROM orders;

-- JOIN
SELECT e.id, e.name, d.name AS dept, e.salary FROM employees e JOIN departments d ON e.dept_id = d.id WHERE e.salary > 70000;
SELECT e.id, e.name, d.name AS dept FROM employees e LEFT JOIN departments d ON e.dept_id = d.id;
SELECT o.id, e.name, o.total FROM orders o JOIN employees e ON o.emp_id = e.id;

-- SUBQUERY
SELECT id, name FROM employees WHERE dept_id IN (SELECT id FROM departments WHERE budget > 300000);
SELECT id, name FROM employees WHERE salary > (SELECT AVG(salary) FROM employees);
SELECT id, name FROM departments WHERE EXISTS (SELECT 1 FROM employees WHERE dept_id = departments.id AND salary > 90000);
SELECT dept_id, avg_sal FROM (SELECT dept_id, AVG(salary) AS avg_sal FROM employees GROUP BY dept_id) AS s WHERE avg_sal > 75000;

-- SCALAR / DATE FUNCTIONS
SELECT id, UPPER(name) AS up, LENGTH(name) AS len, CONCAT(name, '@corp') AS email FROM employees;
SELECT id, SUBSTR(name, 1, 3) AS nick, REPLACE(name, 'Alice', 'Alicia') AS renamed FROM employees;
SELECT id, COALESCE(dept_id, 0) AS dept, IFNULL(dept_id, 0) AS dept2 FROM employees;
SELECT id, DATE_FORMAT(hire_date, '%Y/%m/%d') AS fmt FROM employees;
SELECT CURDATE() AS today, NOW() AS ts FROM departments LIMIT 1;

-- CONSTRAINT VIOLATIONS (expected ERROR)
INSERT INTO employees (name, dept_id, salary) VALUES ('Bad', 1, -500.00);
INSERT INTO orders (emp_id, total, order_date) VALUES (999, 100.00, '2024-01-01');

-- UPDATE + FK CASCADE / SET NULL
UPDATE departments SET name = 'Engineering Team' WHERE id = 1;
SELECT id, name, dept_id FROM employees WHERE dept_id = 1;
UPDATE employees SET salary = 100000.00 WHERE id = 1;

-- DELETE + FK RESTRICT
DELETE FROM orders WHERE id = 1;
SELECT * FROM orders;

-- INDEX + EXPLAIN
CREATE INDEX idx_emp_dept ON employees (dept_id);
EXPLAIN SELECT * FROM employees WHERE dept_id = 1;
EXPLAIN SELECT * FROM employees WHERE id BETWEEN 1 AND 3;
EXPLAIN SELECT * FROM employees WHERE salary > 70000;

-- VIEW
CREATE VIEW v_active AS SELECT id, name, dept_id, salary FROM employees WHERE active = true;
SELECT * FROM v_active WHERE salary > 70000;

-- ALTER TABLE
ALTER TABLE products ADD COLUMN notes TEXT;
ALTER TABLE products MODIFY COLUMN stock INT NOT NULL;
ALTER TABLE products RENAME COLUMN notes TO description;
DESCRIBE products;
ALTER TABLE products DROP COLUMN description;

-- TRANSACTION + SAVEPOINT
BEGIN;
INSERT INTO products (name, price, stock) VALUES ('Tablet', 500.00, 30);
SAVEPOINT sp1;
INSERT INTO products (name, price, stock) VALUES ('Smartwatch', 300.00, 40);
ROLLBACK TO SAVEPOINT sp1;
COMMIT;
SELECT id, name FROM products;

-- TRANSACTION ROLLBACK
BEGIN;
UPDATE employees SET salary = 999999.00 WHERE id = 2;
ROLLBACK;
SELECT id, name, salary FROM employees WHERE id = 2;

-- ISOLATION LEVEL
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;

-- NULL / IS NULL
INSERT INTO employees (name, salary, hire_date) VALUES ('Frank', 55000.00, '2024-03-01');
SELECT id, name, dept_id FROM employees WHERE dept_id IS NULL;

-- COMPOSITE PK
SELECT * FROM order_items;
SELECT order_id, SUM(qty) AS total_qty FROM order_items GROUP BY order_id;

-- SHOW / MONITOR
SHOW TABLES;
DESCRIBE employees;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
CHECKPOINT;
VACUUM;

-- TRUNCATE + DROP
TRUNCATE TABLE order_items;
SELECT COUNT(*) AS cnt FROM order_items;
DROP TABLE order_items;
DROP TABLE orders;
DROP TABLE products;
DROP TABLE employees;
DROP TABLE departments;
DROP VIEW IF EXISTS v_active;
SHOW TABLES;
