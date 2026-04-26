-- test_update_arith.sql

DROP TABLE IF EXISTS products;

CREATE TABLE products (
    id    INT PRIMARY KEY AUTO INCREMENT,
    name  VARCHAR(50),
    price INT,
    stock INT
);

INSERT INTO products (name, price, stock) VALUES
    ('Apple',  100, 50),
    ('Banana',  30, 200),
    ('Cherry', 500, 10);

-- before
SELECT id, name, price, stock FROM products ORDER BY id;

-- 1. 상수 할당 (기존 기능 회귀 테스트)
UPDATE products SET price = 120 WHERE id = 1;
SELECT id, name, price FROM products WHERE id = 1;

-- 2. 자기 참조 산술: price = price * 1.1 (10% 인상)
UPDATE products SET price = price * 2 WHERE id = 2;
SELECT id, name, price FROM products WHERE id = 2;

-- 3. 복합 산술: stock = stock - 5
UPDATE products SET stock = stock - 5 WHERE name = 'Cherry';
SELECT id, name, stock FROM products WHERE id = 3;

-- 4. 여러 컬럼 동시 산술
UPDATE products SET price = price + 50, stock = stock * 2 WHERE id = 1;
SELECT id, name, price, stock FROM products WHERE id = 1;

-- 5. WHERE 없이 전체 갱신 (price 5% 인상)
UPDATE products SET price = price + price / 20;
SELECT id, name, price FROM products ORDER BY id;

-- CLEANUP
DROP TABLE products;
