-- R83: a schema that RuSQL holds and a real mysqldump takes from it (dump_to_mysql.py): comments, a UNIQUE key of two columns, a CHECK, a foreign key, a view, a procedure, functions, triggers.
-- One statement to a line group; DELIMITER is for the mysql client.
CREATE TABLE customers (
  id INT AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(40) NOT NULL COMMENT 'full name',
  email VARCHAR(80) UNIQUE,
  note TEXT,
  balance DECIMAL(10,2) DEFAULT 0.00,
  score DOUBLE,
  joined DATE,
  is_vip TINYINT(1) DEFAULT 0
) COMMENT='the customers';
CREATE TABLE orders (
  id INT AUTO_INCREMENT PRIMARY KEY,
  customer_id INT NOT NULL,
  total DECIMAL(10,2) NOT NULL,
  placed DATETIME,
  status ENUM('new','paid','shipped') DEFAULT 'new',
  tags SET('a','b','c'),
  CONSTRAINT fk_ord_cust FOREIGN KEY (customer_id) REFERENCES customers(id) ON DELETE CASCADE,
  CONSTRAINT ck_total CHECK (total >= 0),
  KEY ix_status (status)
);
CREATE TABLE pairs (a INT, b INT, label VARCHAR(10), UNIQUE KEY ab (a, b));
CREATE TABLE audit (id INT AUTO_INCREMENT PRIMARY KEY, msg VARCHAR(80));
INSERT INTO customers (name, email, note, balance, score, joined, is_vip) VALUES
  ('Ann', 'ann@x.com', 'plain', 10.50, 1.5, '2024-01-31', 1),
  ('O''Brien', 'ob@x.com', 'it''s a "quote"', 0.00, NULL, '2023-12-25', 0),
  ('Back\\slash', NULL, 'line1\nline2', 99999999.99, -3.25, NULL, 0),
  ('한글 이름', 'kr@x.com', '유니코드 😀', 5.00, 2.5, '2020-02-29', 1);
INSERT INTO orders (customer_id, total, placed, status, tags) VALUES
  (1, 120.00, '2024-03-01 10:00:00', 'paid', 'a,b'),
  (1, 15.25, '2024-03-02 11:30:15', 'new', NULL),
  (2, 300.00, NULL, 'shipped', 'c'),
  (4, 0.00, '2024-04-01 00:00:00', 'new', '');
INSERT INTO pairs VALUES (1, 1, 'x'), (1, 2, 'y'), (2, 1, NULL);
CREATE VIEW v_big AS SELECT id, total FROM orders WHERE total > 100;
CREATE VIEW v_cust AS SELECT id, name FROM customers WHERE is_vip = 1;
DELIMITER $$
CREATE PROCEDURE sp_bump(IN cid INT, IN amount DECIMAL(10,2))
BEGIN
  UPDATE customers SET balance = balance + amount WHERE id = cid;
END$$
CREATE PROCEDURE sp_note(IN cid INT) COMMENT 'sets a note' SQL SECURITY INVOKER
BEGIN
  UPDATE customers SET note = CONCAT('noted ', cid) WHERE id = cid;
END$$
CREATE FUNCTION fn_discount(p DECIMAL(10,2)) RETURNS DECIMAL(10,2) DETERMINISTIC
BEGIN
  RETURN p * 0.9;
END$$
CREATE FUNCTION fn_label(s VARCHAR(40)) RETURNS VARCHAR(60) DETERMINISTIC RETURN CONCAT('<', s, '>')$$
CREATE TRIGGER tr_cust_bi BEFORE INSERT ON customers FOR EACH ROW SET NEW.name = TRIM(NEW.name)$$
CREATE TRIGGER tr_order_ai AFTER INSERT ON orders FOR EACH ROW
BEGIN
  INSERT INTO audit (msg) VALUES (CONCAT('order ', NEW.id));
END$$
DELIMITER ;
