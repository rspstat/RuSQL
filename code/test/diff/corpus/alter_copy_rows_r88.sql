# R88 (2026-11-16): the OK packet of ALTER TABLE says how many rows MySQL copied when it copies the table (its ALGORITHM=COPY), and 0 when it does the work in place. Measured on MySQL 8.4.9:
# a column whose type changes (another type, size, precision, signedness, CHAR length, a shorter VARCHAR, a VARCHAR whose length prefix grows, an ENUM / SET not appended, AUTO_INCREMENT added or
# removed), ADD CHECK and ADD FOREIGN KEY copy; renames, nullability / default / comment / position, a longer VARCHAR in the same class, indexes, UNIQUE, ADD / DROP COLUMN do not.
# A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/alter_copy_rows_r88.sql --proto --errno
--CREATE TABLE apk (id INT PRIMARY KEY)
--INSERT INTO apk VALUES (1), (2), (3), (4), (5)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT NOT NULL DEFAULT 5
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT AFTER i
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac DROP COLUMN d
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac DROP COLUMN v10
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v10 VARCHAR(20)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v10 VARCHAR(63)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v70 VARCHAR(100)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY d INT NOT NULL
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY d INT NULL
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY d INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i INT NOT NULL DEFAULT 5
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i INT COMMENT 'x'
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i INT AFTER id
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i INT FIRST
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY en ENUM('a','b','c')
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY st SET('a','b','c')
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY b2 TINYINT(1)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac CHANGE d dd INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac CHANGE v10 w10 VARCHAR(63)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac RENAME COLUMN d TO dd
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD INDEX i2 (v10)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD UNIQUE KEY u2 (v10)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD CONSTRAINT u3 UNIQUE (v10)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ALTER COLUMN i SET DEFAULT 7
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ALTER COLUMN i DROP DEFAULT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac AUTO_INCREMENT = 100
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac COMMENT = 'x'
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac RENAME TO ac2
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, ADD COLUMN y INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, MODIFY v10 VARCHAR(30)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, DROP COLUMN d
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, ADD INDEX i4 (z)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, RENAME TO ac2
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v10 VARCHAR(64)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v63 VARCHAR(64)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v70 VARCHAR(65)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v10 VARCHAR(5)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY ch CHAR(10)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY ch CHAR(3)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY ch VARCHAR(5)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY tx MEDIUMTEXT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY tx VARCHAR(100)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY v10 TEXT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY ti INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i TINYINT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i BIGINT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i INT UNSIGNED
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY ui INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY bi INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i DECIMAL(10,2)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i VARCHAR(20)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dt DATETIME
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dtt DATE
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dtt DATETIME(3)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dtt3 DATETIME(6)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dec1 DECIMAL(12,2)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dec1 DECIMAL(10,3)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY dec1 DOUBLE
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY fl DOUBLE
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY db FLOAT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY en ENUM('b','a')
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY en ENUM('a')
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY st SET('b','a')
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY b2 INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY id INT AUTO_INCREMENT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac CHANGE i j BIGINT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac CHANGE v10 w10 VARCHAR(8)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY i BIGINT, MODIFY v10 VARCHAR(63)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, MODIFY i BIGINT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, MODIFY v10 VARCHAR(5)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac DROP COLUMN ti, MODIFY ui INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD CONSTRAINT ck CHECK (i > 0)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD CHECK (i > 0)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD COLUMN z INT, ADD CHECK (i > 0)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD FOREIGN KEY (d) REFERENCES apk (id)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD CONSTRAINT fk1 FOREIGN KEY (d) REFERENCES apk (id)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac ADD INDEX ii (i), MODIFY bi INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY bl MEDIUMBLOB
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY ts TIMESTAMP(3)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY tm TIME(3)
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY mi INT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY mi MEDIUMINT
--DROP TABLE IF EXISTS ac
--DROP TABLE IF EXISTS ac2
--CREATE TABLE ac (id INT PRIMARY KEY, v10 VARCHAR(10), v63 VARCHAR(63), v70 VARCHAR(70), ch CHAR(5), tx TEXT, ti TINYINT, i INT, bi BIGINT, dt DATE, dtt DATETIME, dtt3 DATETIME(3), dec1 DECIMAL(10,2), fl FLOAT, db DOUBLE, en ENUM('a','b'), st SET('a','b'), ui INT UNSIGNED, b2 BOOLEAN, bl BLOB, ts TIMESTAMP NULL, tm TIME, mi MEDIUMINT, d INT)
--INSERT INTO ac (id, v10, i, bi, ui, d) VALUES (1, 'x', 1, 1, 1, 1), (2, 'y', 2, 2, 2, 2), (3, 'z', 3, 3, 3, 3), (4, 'w', 4, 4, 4, 4), (5, 'u', 5, 5, 5, 5)
ALTER TABLE ac MODIFY bl BLOB NOT NULL
--DROP TABLE IF EXISTS ac
--CREATE TABLE ac (id INT PRIMARY KEY, a INT)
ALTER TABLE ac MODIFY a BIGINT
--DROP TABLE IF EXISTS ac
--CREATE TABLE ac (id INT PRIMARY KEY, a INT)
--INSERT INTO ac VALUES (1, 1), (2, 2), (3, 3)
ALTER TABLE ac MODIFY a BIGINT
SELECT ROW_COUNT()
ALTER TABLE ac ADD COLUMN z INT
SELECT ROW_COUNT()
