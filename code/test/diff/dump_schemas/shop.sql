CREATE TABLE customers (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(50) NOT NULL,
  email VARCHAR(100) DEFAULT NULL,
  city VARCHAR(30) DEFAULT 'Seoul',
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uq_email (email),
  KEY idx_city (city)
) ENGINE=InnoDB;
CREATE TABLE products (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(80) NOT NULL,
  price DECIMAL(10,2) NOT NULL,
  stock INT NOT NULL DEFAULT 0,
  note TEXT,
  PRIMARY KEY (id),
  CONSTRAINT chk_price CHECK (price >= 0)
) ENGINE=InnoDB COMMENT='catalog';
CREATE TABLE orders (
  id INT NOT NULL AUTO_INCREMENT,
  customer_id INT NOT NULL,
  ordered_on DATE NOT NULL,
  status ENUM('new','paid','shipped') NOT NULL DEFAULT 'new',
  PRIMARY KEY (id),
  KEY fk_cust (customer_id),
  CONSTRAINT fk_orders_cust FOREIGN KEY (customer_id) REFERENCES customers (id) ON DELETE CASCADE
) ENGINE=InnoDB;
CREATE TABLE order_items (
  order_id INT NOT NULL,
  product_id INT NOT NULL,
  qty INT NOT NULL,
  PRIMARY KEY (order_id, product_id),
  CONSTRAINT fk_oi_o FOREIGN KEY (order_id) REFERENCES orders (id),
  CONSTRAINT fk_oi_p FOREIGN KEY (product_id) REFERENCES products (id)
) ENGINE=InnoDB;
INSERT INTO customers (name, email, city) VALUES ('Alice', 'alice@x.com', 'Seoul'), ('Bob', 'bob@x.com', 'Busan'), ('Carol', NULL, 'Seoul'), ('한글이름', 'h@x.com', '서울'), ('O''Brien', 'ob@x.com', 'Daegu');
INSERT INTO products (name, price, stock, note) VALUES ('Pen', 1.50, 100, NULL), ('Book', 12.00, 20, 'a "quoted" note\nsecond line'), ('Lamp', 30.25, 0, '');
INSERT INTO orders (customer_id, ordered_on, status) VALUES (1, '2024-01-05', 'paid'), (1, '2024-02-10', 'new'), (2, '2024-03-01', 'shipped');
INSERT INTO order_items VALUES (1, 1, 3), (1, 2, 1), (2, 3, 2), (3, 1, 10);
CREATE VIEW order_totals AS SELECT o.id AS order_id, SUM(oi.qty * p.price) AS total FROM orders o JOIN order_items oi ON oi.order_id = o.id JOIN products p ON p.id = oi.product_id GROUP BY o.id;
DELIMITER ;;
CREATE PROCEDURE add_stock(IN pid INT, IN n INT)
BEGIN
  UPDATE products SET stock = stock + n WHERE id = pid;
END;;
CREATE TRIGGER trg_ord_ins BEFORE INSERT ON orders FOR EACH ROW
BEGIN
  SET NEW.status = IFNULL(NEW.status, 'new');
END;;
DELIMITER ;
