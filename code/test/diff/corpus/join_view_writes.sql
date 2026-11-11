# UPDATE and INSERT through a view over a join (R80), run with --proto --errno. MySQL writes through it when the statement changes the columns of ONE base table: an UPDATE is a multi-table UPDATE
# (each row of the table once, the WHERE of the view and of the statement both apply), an INSERT needs a list of columns of one table, an outer join takes neither, DELETE and REPLACE are refused.
# (not here: WITH CHECK OPTION on a join view, and a new value that reads a column of the view that is an expression -- both refused with a message of their own)
--CREATE TABLE ja (id INT PRIMARY KEY, x INT, g INT)
--CREATE TABLE jb (id INT PRIMARY KEY, y INT, aid INT, z INT)
--INSERT INTO ja VALUES (1,10,1),(2,20,1),(3,30,2),(4,40,NULL)
--INSERT INTO jb VALUES (1,100,1,5),(2,200,1,6),(3,300,2,7),(4,400,9,8)
--CREATE VIEW v1 AS SELECT ja.id AS aid, ja.x, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id
--CREATE VIEW v2 AS SELECT ja.id AS aid, ja.x, jb.y FROM ja LEFT JOIN jb ON jb.aid = ja.id
--CREATE VIEW v3 AS SELECT ja.id AS aid, ja.x, jb.id AS bid, jb.y FROM ja RIGHT JOIN jb ON jb.aid = ja.id
--CREATE VIEW v4 AS SELECT ja.id AS aid, ja.x, jb.y FROM ja JOIN jb ON jb.aid = ja.id WHERE ja.g = 1
--CREATE VIEW v5 AS SELECT ja.id AS aid, ja.x * 2 AS dbl, jb.y FROM ja JOIN jb ON jb.aid = ja.id
--CREATE VIEW v6 AS SELECT a.id, a.x, b.y FROM ja a JOIN jb b ON b.aid = a.id
--CREATE VIEW v7 AS SELECT * FROM v1 WHERE y > 100
--CREATE VIEW v9 AS SELECT ja.id, ja.x, jb.id AS bid, jb.y FROM ja, jb WHERE jb.aid = ja.id
# UPDATE: the columns of one table
UPDATE v1 SET x = x + 1 WHERE y = 100
SELECT id, x FROM ja ORDER BY id
UPDATE v1 SET x = x + 1 WHERE aid = 1
SELECT id, x FROM ja ORDER BY id
UPDATE v1 SET x = 0 WHERE aid = 2
SELECT id, x FROM ja ORDER BY id
UPDATE v1 SET y = y + 1 WHERE bid = 3
SELECT id, y FROM jb ORDER BY id
UPDATE v1 SET x = x + 100, aid = aid + 100 WHERE aid = 3
UPDATE v1 SET x = 1, y = 2
UPDATE v1 SET nosuch = 1
UPDATE v1 SET x = 5 WHERE nosuch = 1
UPDATE v1 SET x = y WHERE aid = 3
UPDATE v1 SET x = y WHERE aid = 2
SELECT id, x FROM ja ORDER BY id
UPDATE v1 SET aid = 33 WHERE aid = 3
UPDATE v1 SET x = 1 WHERE aid = 99
UPDATE v1 SET x = 7 ORDER BY aid LIMIT 1
UPDATE v1 SET x = 7 LIMIT 1
# outer joins take no UPDATE (1288)
UPDATE v2 SET x = 41 WHERE aid = 4
UPDATE v2 SET y = 1 WHERE aid = 1
UPDATE v3 SET y = 1 WHERE bid = 4
UPDATE v3 SET x = 1 WHERE bid = 1
# the WHERE of the view, an expression column (1348), an alias of a table, a view over a join view, a comma join
UPDATE v4 SET x = 77
SELECT id, x FROM ja ORDER BY id
UPDATE v5 SET dbl = 1
UPDATE v5 SET y = 9 WHERE aid = 1
SELECT id, y FROM jb ORDER BY id
UPDATE v6 SET x = 3 WHERE id = 1
UPDATE v7 SET x = 55 WHERE bid = 2
UPDATE v7 SET x = 56 WHERE bid = 1
SELECT id, x FROM ja ORDER BY id
UPDATE v9 SET x = 8 WHERE id = 1
UPDATE v9 SET y = y + 5 WHERE bid = 4
SELECT id, x FROM ja ORDER BY id
SELECT id, y FROM jb ORDER BY id
SELECT id, x FROM ja ORDER BY id
# INSERT: a list of the columns of one table
INSERT INTO v1 (aid, x) VALUES (10, 1)
SELECT id, x, g FROM ja ORDER BY id
INSERT INTO v1 (x) VALUES (1)
INSERT INTO v1 (aid, x, y) VALUES (11, 1, 2)
INSERT INTO v1 (bid, y) VALUES (50, 1)
SELECT id, y, aid FROM jb ORDER BY id
INSERT INTO v1 VALUES (12, 1, 51, 2)
INSERT INTO v1 (aid, x) VALUES (13, 1), (14, 2)
INSERT INTO v1 (aid, nosuch) VALUES (15, 1)
INSERT INTO v2 (aid, x) VALUES (15, 1)
INSERT INTO v2 (y) VALUES (1)
INSERT INTO v3 (bid, y) VALUES (60, 1)
INSERT INTO v3 (aid, x) VALUES (16, 1)
INSERT INTO v4 (aid, x) VALUES (17, 1)
INSERT INTO v5 (aid, dbl) VALUES (18, 1)
INSERT INTO v5 (aid) VALUES (18)
INSERT INTO v6 (id, x) VALUES (19, 1)
INSERT INTO v7 (aid, x) VALUES (22, 1)
INSERT INTO v9 (id, x) VALUES (23, 1)
INSERT INTO v1 (aid, x) SELECT 21, 1
INSERT INTO v1 (aid, x) SELECT id + 100, x FROM ja WHERE id < 3
REPLACE INTO v1 (aid, x) VALUES (10, 2)
INSERT INTO v1 (aid, x) VALUES (10, 3) ON DUPLICATE KEY UPDATE x = 4
INSERT INTO v6 (id, x) VALUES (19, 3) ON DUPLICATE KEY UPDATE x = x + 100
INSERT INTO v1 (aid, x) VALUES (10, 3) ON DUPLICATE KEY UPDATE aid = 24
SELECT id, x FROM ja WHERE id >= 10 ORDER BY id
# DELETE (1395)
DELETE FROM v1 WHERE aid = 1
DELETE FROM v9 WHERE id = 1
DELETE FROM v2 WHERE aid = 1
SELECT COUNT(*) FROM ja
SELECT COUNT(*) FROM jb
