CREATE TABLE kinds (
  id bigint unsigned NOT NULL AUTO_INCREMENT,
  flag tinyint(1) NOT NULL DEFAULT '0',
  small smallint DEFAULT NULL,
  big bigint DEFAULT '9007199254740993',
  ratio double DEFAULT NULL,
  price decimal(12,4) NOT NULL DEFAULT '0.0000',
  label char(8) DEFAULT NULL,
  title varchar(100) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL,
  body longtext,
  raw blob,
  kind enum('a','b','c') NOT NULL DEFAULT 'a',
  born date DEFAULT NULL,
  seen datetime DEFAULT NULL,
  stamp timestamp NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  doc json DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uq_title (title),
  KEY idx_born (born),
  KEY idx_kind_flag (kind, flag)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
INSERT INTO kinds (flag, small, big, ratio, price, label, title, body, kind, born, seen, stamp, doc) VALUES
 (1, -5, 5, 1.5e3, 12.3456, 'abc', 'First', 'line1\nline2\ttabbed', 'b', '2024-02-29', '2024-02-29 13:14:15', '2024-02-29 13:14:15', '{"a": [1, 2, {"b": null}]}'),
 (0, NULL, NULL, -0.25, 0, NULL, 'second', '', 'c', NULL, NULL, '2000-01-01 00:00:00', NULL),
 (1, 32767, 9007199254740993, 3.14159, 99999999.9999, 'x|y', 'Pipe|Bar', 'back\\slash and ''quote'' and "double"', 'a', '1999-12-31', '1999-12-31 23:59:59', '2001-01-01 00:00:00', '[]');
CREATE TABLE pairs (
  a int NOT NULL,
  b int NOT NULL,
  note varchar(20) DEFAULT NULL,
  PRIMARY KEY (a, b),
  UNIQUE KEY uq_note (note, a)
) ENGINE=InnoDB;
INSERT INTO pairs VALUES (1, 1, 'x'), (1, 2, 'y'), (2, 1, NULL), (2, 2, NULL);
CREATE TABLE tree (
  id int NOT NULL,
  parent int DEFAULT NULL,
  name varchar(20) NOT NULL,
  PRIMARY KEY (id),
  CONSTRAINT fk_tree_parent FOREIGN KEY (parent) REFERENCES tree (id) ON DELETE CASCADE ON UPDATE CASCADE
) ENGINE=InnoDB;
INSERT INTO tree VALUES (1, NULL, 'root'), (2, 1, 'child'), (3, 2, 'leaf');
CREATE TABLE audit (id int NOT NULL AUTO_INCREMENT, what varchar(50), at timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY (id));
DELIMITER ;;
CREATE TRIGGER trg_tree_ins AFTER INSERT ON tree FOR EACH ROW
BEGIN
  INSERT INTO audit (what) VALUES (CONCAT('tree ', NEW.id));
END;;
CREATE FUNCTION depth_of(n int) RETURNS int DETERMINISTIC
BEGIN
  RETURN n + 1;
END;;
DELIMITER ;
INSERT INTO tree VALUES (4, 3, 'deeper');
