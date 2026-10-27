# A column in several foreign keys (R57): each key is checked on its own -- a value has to be in every parent --, each key acts on its own when a parent row goes, and each is listed under its
# own name (<table>_ibfk_1, _2). (A column used to keep the last key it was given and lose the earlier ones without a word.) Every scenario has tables of its own.
--CREATE TABLE mf_a (id INT PRIMARY KEY)
--CREATE TABLE mf_b (id INT PRIMARY KEY)
--INSERT INTO mf_a VALUES (1),(2),(5)
--INSERT INTO mf_b VALUES (2),(3),(5)
--CREATE TABLE mf_c (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES mf_a(id), FOREIGN KEY (x) REFERENCES mf_b(id))
INSERT INTO mf_c VALUES (1, 1)
INSERT INTO mf_c VALUES (2, 3)
INSERT INTO mf_c VALUES (3, 2)
INSERT INTO mf_c VALUES (4, NULL)
INSERT INTO mf_c VALUES (5, 5)
INSERT INTO mf_c VALUES (6, 9)
SELECT id, x FROM mf_c ORDER BY id
UPDATE mf_c SET x = 1 WHERE id = 3
UPDATE mf_c SET x = 3 WHERE id = 3
UPDATE mf_c SET x = 5 WHERE id = 3
SELECT id, x FROM mf_c ORDER BY id
DELETE FROM mf_a WHERE id = 5
DELETE FROM mf_b WHERE id = 5
DELETE FROM mf_a WHERE id = 1
SELECT id FROM mf_a ORDER BY id
SELECT id FROM mf_b ORDER BY id
SELECT CONSTRAINT_NAME, COLUMN_NAME, REFERENCED_TABLE_NAME FROM information_schema.key_column_usage WHERE TABLE_NAME = 'mf_c' AND REFERENCED_TABLE_NAME IS NOT NULL ORDER BY CONSTRAINT_NAME
SELECT CONSTRAINT_NAME, REFERENCED_TABLE_NAME FROM information_schema.referential_constraints WHERE TABLE_NAME = 'mf_c' ORDER BY CONSTRAINT_NAME
SELECT CONSTRAINT_NAME, CONSTRAINT_TYPE FROM information_schema.table_constraints WHERE TABLE_NAME = 'mf_c' ORDER BY CONSTRAINT_NAME
ALTER TABLE mf_c DROP FOREIGN KEY mf_c_ibfk_1
INSERT INTO mf_c VALUES (7, 3)
INSERT INTO mf_c VALUES (8, 4)
SELECT id, x FROM mf_c ORDER BY id
# the keys act on their own: one cascades, the other nulls the column
--CREATE TABLE mg_a (id INT PRIMARY KEY)
--CREATE TABLE mg_b (id INT PRIMARY KEY)
--INSERT INTO mg_a VALUES (1),(2),(3)
--INSERT INTO mg_b VALUES (1),(2),(3)
--CREATE TABLE mg_c (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES mg_a(id) ON DELETE CASCADE, FOREIGN KEY (x) REFERENCES mg_b(id) ON DELETE SET NULL)
--INSERT INTO mg_c VALUES (10, 1), (20, 2), (30, 3)
DELETE FROM mg_b WHERE id = 1
SELECT id, x FROM mg_c ORDER BY id
DELETE FROM mg_a WHERE id = 2
SELECT id, x FROM mg_c ORDER BY id
DELETE FROM mg_b WHERE id = 3
DELETE FROM mg_a WHERE id = 3
SELECT id, x FROM mg_c ORDER BY id
# the new value that ON UPDATE CASCADE writes into the column has to be in the other parent too
--CREATE TABLE mj_a (id INT PRIMARY KEY)
--CREATE TABLE mj_b (id INT PRIMARY KEY)
--INSERT INTO mj_a VALUES (1),(2)
--INSERT INTO mj_b VALUES (1),(7)
--CREATE TABLE mj_c (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES mj_a(id) ON UPDATE CASCADE, FOREIGN KEY (x) REFERENCES mj_b(id) ON UPDATE CASCADE)
--INSERT INTO mj_c VALUES (10, 1)
UPDATE mj_a SET id = 5 WHERE id = 1
SELECT id FROM mj_a ORDER BY id
SELECT id, x FROM mj_c ORDER BY id
UPDATE mj_a SET id = 7 WHERE id = 1
SELECT id FROM mj_a ORDER BY id
SELECT id, x FROM mj_c ORDER BY id
UPDATE mj_b SET id = 9 WHERE id = 7
SELECT id FROM mj_b ORDER BY id
SELECT id, x FROM mj_c ORDER BY id
# both keys cascade
--CREATE TABLE mh_a (id INT PRIMARY KEY)
--CREATE TABLE mh_b (id INT PRIMARY KEY)
--INSERT INTO mh_a VALUES (1),(2)
--INSERT INTO mh_b VALUES (1),(2)
--CREATE TABLE mh_c (id INT PRIMARY KEY, x INT, FOREIGN KEY (x) REFERENCES mh_a(id) ON DELETE CASCADE, FOREIGN KEY (x) REFERENCES mh_b(id) ON DELETE CASCADE)
--INSERT INTO mh_c VALUES (10, 1), (20, 2)
DELETE FROM mh_a WHERE id = 1
SELECT id, x FROM mh_c ORDER BY id
DELETE FROM mh_b WHERE id = 2
SELECT id, x FROM mh_c ORDER BY id
# the second key added by ALTER TABLE, with a name of its own
--CREATE TABLE mi_a (id INT PRIMARY KEY)
--CREATE TABLE mi_b (id INT PRIMARY KEY)
--INSERT INTO mi_a VALUES (1),(2)
--INSERT INTO mi_b VALUES (1),(2)
--CREATE TABLE mi_c (id INT PRIMARY KEY, x INT, CONSTRAINT first_key FOREIGN KEY (x) REFERENCES mi_a(id))
ALTER TABLE mi_c ADD CONSTRAINT second_key FOREIGN KEY (x) REFERENCES mi_b(id)
ALTER TABLE mi_c ADD CONSTRAINT second_key FOREIGN KEY (x) REFERENCES mi_b(id)
INSERT INTO mi_c VALUES (1, 1)
INSERT INTO mi_c VALUES (2, 3)
SELECT CONSTRAINT_NAME, REFERENCED_TABLE_NAME FROM information_schema.referential_constraints WHERE TABLE_NAME = 'mi_c' ORDER BY CONSTRAINT_NAME
ALTER TABLE mi_c DROP FOREIGN KEY first_key
INSERT INTO mi_c VALUES (3, 2)
SELECT id, x FROM mi_c ORDER BY id
DROP TABLE mi_b
DROP TABLE mi_c
DROP TABLE mi_b
