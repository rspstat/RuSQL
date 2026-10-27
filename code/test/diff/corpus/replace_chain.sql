# REPLACE and the foreign keys below the row it replaces (R57): REPLACE deletes the old row and inserts the new one, and the delete goes down the whole chain of tables like any DELETE -- the
# children that go with the row, the grandchildren that go with them -- or is refused, when a RESTRICT is anywhere below, with nothing changed: not the rows the statement replaced before it
# came to the protected one. (A protected row two tables down used to be found only when the delete was made: an earlier row of the statement had been deleted by then and was not inserted.)
--CREATE TABLE rc_a (id INT PRIMARY KEY, v INT)
--CREATE TABLE rc_b (id INT PRIMARY KEY, a INT, FOREIGN KEY (a) REFERENCES rc_a(id) ON DELETE CASCADE)
--CREATE TABLE rc_c (id INT PRIMARY KEY, b INT, FOREIGN KEY (b) REFERENCES rc_b(id) ON DELETE RESTRICT)
--INSERT INTO rc_a VALUES (1, 10), (2, 20), (3, 30)
--INSERT INTO rc_b VALUES (11, 1)
--INSERT INTO rc_c VALUES (111, 11)
REPLACE INTO rc_a VALUES (2, 21), (1, 11)
SELECT id, v FROM rc_a ORDER BY id
SELECT COUNT(*) FROM rc_b
SELECT COUNT(*) FROM rc_c
REPLACE INTO rc_a VALUES (1, 11), (2, 21)
SELECT id, v FROM rc_a ORDER BY id
REPLACE INTO rc_a VALUES (3, 31)
SELECT id, v FROM rc_a ORDER BY id
REPLACE INTO rc_a VALUES (4, 40), (3, 32), (2, 22)
SELECT id, v FROM rc_a ORDER BY id
REPLACE INTO rc_a SELECT id, v + 1 FROM rc_a WHERE id IN (2, 1)
SELECT id, v FROM rc_a ORDER BY id
REPLACE INTO rc_a SELECT id, v + 1 FROM rc_a WHERE id IN (2, 3)
SELECT id, v FROM rc_a ORDER BY id
REPLACE INTO rc_a SET id = 1, v = 99
SELECT id, v FROM rc_a ORDER BY id
SELECT COUNT(*) FROM rc_b
DELETE FROM rc_c
REPLACE INTO rc_a VALUES (2, 23), (1, 13)
SELECT id, v FROM rc_a ORDER BY id
SELECT COUNT(*) FROM rc_b
SELECT COUNT(*) FROM rc_c
# every table of the chain cascades: the grandchildren go with the row that is replaced
--CREATE TABLE rd_a (id INT PRIMARY KEY, v INT)
--CREATE TABLE rd_b (id INT PRIMARY KEY, a INT, FOREIGN KEY (a) REFERENCES rd_a(id) ON DELETE CASCADE)
--CREATE TABLE rd_c (id INT PRIMARY KEY, b INT, FOREIGN KEY (b) REFERENCES rd_b(id) ON DELETE CASCADE)
--INSERT INTO rd_a VALUES (1, 10), (2, 20)
--INSERT INTO rd_b VALUES (11, 1), (22, 2)
--INSERT INTO rd_c VALUES (111, 11), (222, 22)
REPLACE INTO rd_a VALUES (1, 11), (3, 30)
SELECT id, v FROM rd_a ORDER BY id
SELECT COUNT(*) FROM rd_b
SELECT COUNT(*) FROM rd_c
# a SET NULL in the middle of the chain, a RESTRICT below it
--CREATE TABLE re_a (id INT PRIMARY KEY, v INT)
--CREATE TABLE re_b (id INT PRIMARY KEY, a INT, FOREIGN KEY (a) REFERENCES re_a(id) ON DELETE SET NULL)
--CREATE TABLE re_c (id INT PRIMARY KEY, b INT, FOREIGN KEY (b) REFERENCES re_b(id) ON DELETE RESTRICT)
--INSERT INTO re_a VALUES (1, 10), (2, 20)
--INSERT INTO re_b VALUES (11, 1), (22, 2)
--INSERT INTO re_c VALUES (111, 11)
REPLACE INTO re_a VALUES (2, 21), (1, 11)
SELECT id, v FROM re_a ORDER BY id
SELECT id, a FROM re_b ORDER BY id
# a table that is its own child
--CREATE TABLE rf (id INT PRIMARY KEY, up INT, FOREIGN KEY (up) REFERENCES rf(id) ON DELETE RESTRICT)
--INSERT INTO rf VALUES (1, NULL), (2, 1), (3, NULL)
REPLACE INTO rf VALUES (3, NULL), (1, NULL)
SELECT id, up FROM rf ORDER BY id
REPLACE INTO rf VALUES (3, 1), (2, 3)
SELECT id, up FROM rf ORDER BY id
