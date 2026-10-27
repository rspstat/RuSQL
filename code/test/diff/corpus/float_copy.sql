# FLOAT values that are read back and copied (R57). A FLOAT column holds a single-precision number and is SHOWN with six digits (16777217 is 16777200), also when MIN / MAX / GREATEST /
# LEAST / COALESCE / IFNULL / NULLIF pick it (a function over FLOAT columns, whole-number constants and NULL is a FLOAT). Copied into a column that is not a FLOAT it is the number it
# holds, widened to a DOUBLE (0.1 is 0.10000000149011612 there, 16777217 is 16777216); copied into a FLOAT it stays the float.
--CREATE TABLE fc (id INT PRIMARY KEY, f FLOAT, d DOUBLE, i INT, dc DECIMAL(20,4), t VARCHAR(40))
--INSERT INTO fc VALUES (1, 1234567, 1, 1, 1, 'a'), (2, 16777217, 2, 2, 2, 'b'), (3, 0.1, 3, 3, 3, 'c'), (4, 123456789, 4, 4, 4, 'd'), (5, NULL, 5, 5, 5, 'e'), (6, 3.14159265, 6, 6, 6, 'f'), (7, -2.5, 7, 7, 7, 'g'), (8, 0, 8, 8, 8, 'h')
SELECT f FROM fc ORDER BY id
SELECT MIN(f), MAX(f) FROM fc
SELECT MAX(f) AS mx, MIN(f) AS mn FROM fc
SELECT id, MAX(f) FROM fc GROUP BY id ORDER BY id
SELECT MAX(f) FROM fc WHERE id = 2
SELECT MIN(f) FROM fc WHERE id > 100
SELECT COUNT(*), MAX(f) FROM fc WHERE id <= 2
SELECT GREATEST(f, 0), LEAST(f, 100000000) FROM fc ORDER BY id
SELECT COALESCE(f, 0), IFNULL(f, 0), NULLIF(f, 0) FROM fc ORDER BY id
SELECT COALESCE(f, NULL) FROM fc ORDER BY id
SELECT COALESCE(f, 5) AS c FROM fc ORDER BY id
SELECT GREATEST(f, 0) + 1 FROM fc WHERE id IN (1, 2, 5) ORDER BY id
SELECT f + 0, f * 1 FROM fc ORDER BY id
--CREATE TABLE fc_d (id INT PRIMARY KEY, x DOUBLE)
--CREATE TABLE fc_f (id INT PRIMARY KEY, x FLOAT)
--CREATE TABLE fc_n (id INT PRIMARY KEY, x DECIMAL(20,4))
--CREATE TABLE fc_i (id INT PRIMARY KEY, x INT)
--CREATE TABLE fc_t (id INT PRIMARY KEY, x VARCHAR(40))
INSERT INTO fc_d SELECT id, f FROM fc
SELECT id, x FROM fc_d ORDER BY id
INSERT INTO fc_f SELECT id, f FROM fc
SELECT id, x FROM fc_f ORDER BY id
INSERT INTO fc_n SELECT id, f FROM fc
SELECT id, x FROM fc_n ORDER BY id
INSERT INTO fc_i SELECT id, f FROM fc WHERE id <> 7
SELECT id, x FROM fc_i ORDER BY id
INSERT INTO fc_t SELECT id, f FROM fc
SELECT id, x FROM fc_t ORDER BY id
--CREATE TABLE fc_e (id INT PRIMARY KEY, x DOUBLE)
INSERT INTO fc_e SELECT id, COALESCE(f, 0) FROM fc
SELECT id, x FROM fc_e ORDER BY id
--CREATE TABLE fc_m (x DOUBLE)
INSERT INTO fc_m SELECT MAX(f) FROM fc
INSERT INTO fc_m SELECT MIN(f) FROM fc
SELECT x FROM fc_m ORDER BY x
UPDATE fc SET d = f
SELECT id, d FROM fc ORDER BY id
UPDATE fc SET dc = f
SELECT id, dc FROM fc ORDER BY id
UPDATE fc SET i = f WHERE id <> 7
SELECT id, i FROM fc ORDER BY id
UPDATE fc SET t = f
SELECT id, t FROM fc ORDER BY id
UPDATE fc SET d = COALESCE(f, 0)
SELECT id, d FROM fc ORDER BY id
UPDATE fc SET d = 0
UPDATE fc SET d = f + 0
SELECT id, d FROM fc ORDER BY id
SELECT id FROM fc WHERE d = f ORDER BY id
SELECT id FROM fc WHERE f = 0.1
