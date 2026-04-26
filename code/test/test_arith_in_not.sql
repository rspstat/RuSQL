-- test_arith_in_not.sql
-- Tests: IN (literal list), Arithmetic expressions, NOT condition

DROP TABLE IF EXISTS products;
DROP TABLE IF EXISTS orders;

CREATE TABLE products (
    id    INT PRIMARY KEY AUTO INCREMENT,
    name  VARCHAR(50),
    price INT,
    qty   INT,
    active INT
);

CREATE TABLE orders (
    order_id   INT PRIMARY KEY AUTO INCREMENT,
    product_id INT,
    amount     INT
);

INSERT INTO products (name, price, qty, active) VALUES
    ('Apple',  100, 5,  1),
    ('Banana',  30, 20, 1),
    ('Cherry', 500, 2,  0),
    ('Date',   200, 8,  1),
    ('Elder',   50, 0,  0);

INSERT INTO orders (product_id, amount) VALUES
    (1, 3),
    (2, 10),
    (3, 1),
    (4, 2);

-- 1. IN (literal list): integer values
SELECT id, name FROM products WHERE id IN (1, 3, 5);

-- 2. IN (literal list): string values
SELECT id, name FROM products WHERE name IN ('Apple', 'Date', 'Elder');

-- 3. NOT IN (literal list)
SELECT id, name FROM products WHERE id NOT IN (2, 4);

-- 4. Arithmetic in SELECT: price * qty AS total
SELECT id, name, price * qty AS total FROM products ORDER BY id;

-- 5. Arithmetic in WHERE: price * qty > 400
SELECT id, name, price * qty AS total FROM products WHERE price * qty > 400;

-- 6. Arithmetic: addition in SELECT
SELECT id, name, price + qty AS price_plus_qty FROM products ORDER BY id;

-- 7. Arithmetic: division in SELECT (price / qty)
SELECT id, name, price / qty AS unit_cost FROM products WHERE qty > 0 ORDER BY id;

-- 8. NOT simple condition: NOT active = 1  →  active != 1
SELECT id, name FROM products WHERE NOT (active = 1);

-- 9. NOT compound: NOT (price > 100 OR active = 0)
SELECT id, name FROM products WHERE NOT (price > 100 OR active = 0);

-- 10. Arithmetic in WHERE with IN: price IN (100, 200, 500)
SELECT name, price FROM products WHERE price IN (100, 200, 500);

-- 11. Combined: arithmetic WHERE + NOT
SELECT id, name, price * qty AS revenue
FROM products
WHERE price * qty > 0 AND NOT (active = 0)
ORDER BY revenue DESC;

-- 12. Arithmetic expression in SELECT without alias (header shows expr)
SELECT id, price - 10 FROM products WHERE id IN (1, 2) ORDER BY id;

-- CLEANUP
DROP TABLE orders;
DROP TABLE products;
