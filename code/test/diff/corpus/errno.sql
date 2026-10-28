# The error number a client gets (R58, run with --proto --errno): the MySQL protocol told every error by 1064, a syntax error. A client tells errors by their number (1062 duplicate entry,
# 1451 / 1452 foreign key, 1054 unknown column ...), so both servers must answer the same number for a statement that fails on both.
--CREATE TABLE en_p (id INT PRIMARY KEY, name VARCHAR(5) NOT NULL, n INT, d DATE, e ENUM('a','b'))
--CREATE TABLE en_c (id INT PRIMARY KEY, pid INT, q INT, u INT UNIQUE, CONSTRAINT en_fk FOREIGN KEY (pid) REFERENCES en_p(id), CONSTRAINT en_ck CHECK (q < 100))
--INSERT INTO en_p VALUES (1, 'a', 1, '2024-01-01', 'a')
--INSERT INTO en_c VALUES (1, 1, 5, 1), (5, 1, 6, 5)
INSERT INTO en_p VALUES (1, 'x', 1, '2024-01-01', 'a')
INSERT INTO en_p VALUES (2, NULL, 1, '2024-01-01', 'a')
INSERT INTO en_p (id) VALUES (3)
INSERT INTO en_p VALUES (4, 'toolong', 1, '2024-01-01', 'a')
INSERT INTO en_p VALUES (5, 'x', 99999999999, '2024-01-01', 'a')
INSERT INTO en_p VALUES (6, 'x', 'abc', '2024-01-01', 'a')
INSERT INTO en_p VALUES (7, 'x', '12abc', '2024-01-01', 'a')
INSERT INTO en_p VALUES (8, 'x', 1, '2024-02-30', 'a')
INSERT INTO en_p VALUES (9, 'x', 1, '2024-01-01', 'z')
INSERT INTO en_p VALUES (10, 'x', 1/0, '2024-01-01', 'a')
INSERT INTO en_p VALUES (1)
INSERT INTO en_p SET id = 21, id = 22
INSERT INTO en_c VALUES (2, 99, 5, 2)
INSERT INTO en_c VALUES (3, 1, 500, 3)
INSERT INTO en_c VALUES (4, 1, 5, 1)
DELETE FROM en_p WHERE id = 1
UPDATE en_p SET id = 7 WHERE id = 1
UPDATE en_c SET pid = 99 WHERE id = 1
DROP TABLE en_p
SELECT nope FROM en_p
SELECT COUNT(*) FROM en_p GROUP BY nope
SELECT * FROM nosuch
UPDATE nosuch SET a = 1
CREATE TABLE en_p (a INT)
DROP TABLE nosuch
CREATE TABLE en_dup (a INT, a INT)
CREATE INDEX en_i ON en_p (n)
CREATE INDEX en_i ON en_p (n)
ALTER TABLE en_p DROP COLUMN nosuch
ALTER TABLE en_p ADD COLUMN n INT
ALTER TABLE en_c DROP FOREIGN KEY nosuch
SELECT id FROM en_p, en_c
SELECT p.id FROM en_p p JOIN en_p p ON p.id = p.id
UPDATE en_p SET n = 1 WHERE id IN (SELECT id FROM en_p)
SELECT (SELECT id FROM en_c)
SELECT (SELECT 1, 2)
SELECT SUM(SUM(n)) FROM en_p
SELECT CONCAT()
SELECT 9223372036854775807 + 1
SELECT NOW(7)
SELECT SUM(n) OVER nosuch FROM en_p
SELECT 'a' LIKE 'a' ESCAPE '!!'
CALL nosuch()
CREATE DATABASE v
USE nosuchdb
DROP DATABASE nosuchdb
SELEC 1
SELECT FROM
SELECT 1 +
