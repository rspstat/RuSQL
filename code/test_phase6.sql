-- Phase 6 통합 테스트
-- 1. 새 데이터 타입
DROP TABLE IF EXISTS products;
CREATE TABLE products (id INT PRIMARY KEY AUTO INCREMENT, name VARCHAR(100) NOT NULL, price DECIMAL(10,2), created_at DATE);
INSERT INTO products (name, price, created_at) VALUES ('Widget', 9.99, '2024-01-15');
INSERT INTO products (name, price, created_at) VALUES ('Gadget', 24.99, '2024-02-20');
DESCRIBE products;
SELECT * FROM products;
-- 2. 문자열 함수
SELECT id, UPPER(name) AS upper_name, LOWER(name) AS lower_name, LENGTH(name) AS len FROM products;
SELECT id, CONCAT(name, ' - $', price) AS label, SUBSTR(name, 1, 3) AS short FROM products;
SELECT id, REPLACE(name, 'Widget', 'Sprocket') AS replaced FROM products;
SELECT id, IFNULL(created_at, '2000-01-01') AS safe_date FROM products;
-- 3. 날짜 함수
SELECT id, DATE_FORMAT(created_at, '%Y/%m/%d') AS fmt_date FROM products;
SELECT CURDATE() AS today FROM products;
-- 4. CHECK 제약
DROP TABLE IF EXISTS accounts;
CREATE TABLE accounts (id INT PRIMARY KEY, balance DECIMAL(10,2) CHECK (balance >= 0));
INSERT INTO accounts VALUES (1, 100.00);
INSERT INTO accounts VALUES (2, -50.00);
SELECT * FROM accounts;
-- 5. 복합 PK
DROP TABLE IF EXISTS order_items;
CREATE TABLE order_items (order_id INT, product_id INT, qty INT, PRIMARY KEY (order_id, product_id));
INSERT INTO order_items VALUES (1, 101, 2);
INSERT INTO order_items VALUES (1, 102, 5);
DESCRIBE order_items;
SELECT * FROM order_items;
-- 6. COALESCE / IFNULL
DROP TABLE IF EXISTS users;
CREATE TABLE users (id INT PRIMARY KEY, name TEXT, nickname VARCHAR(50));
INSERT INTO users VALUES (1, 'Alice', NULL);
INSERT INTO users VALUES (2, 'Bob', 'Bobby');
SELECT id, name, IFNULL(nickname, name) AS display_name FROM users;
SELECT id, COALESCE(nickname, name) AS display FROM users;
