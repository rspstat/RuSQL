# R86 (2026-11-15): a write through a view over a JOIN that reads a column of the view that is an expression (`ja.x * 2 AS dbl`, `ja.x + jb.y AS s`, a CASE): MySQL puts the expression in the place
# of the column in the WHERE and in a new value (R80 refused it). A line is a statement, '--' a setup statement run on both.
# Run: python vs_mysql.py <engine_server.exe> corpus/join_view_expr_r86.sql --proto --errno
--CREATE TABLE ja (id INT PRIMARY KEY, x INT, g INT)
--CREATE TABLE jb (id INT PRIMARY KEY, y INT, aid INT, z INT)
--INSERT INTO ja VALUES (1,10,1),(2,20,1),(3,30,2),(4,40,NULL)
--INSERT INTO jb VALUES (1,100,1,5),(2,200,1,6),(3,300,2,7),(4,400,9,8)
--CREATE VIEW jx1 AS SELECT ja.id AS aid, ja.x * 2 AS dbl, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id
--CREATE VIEW jx2 AS SELECT ja.id AS aid, ja.x + jb.y AS s, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id
--CREATE VIEW jx3 AS SELECT ja.id AS aid, CASE WHEN ja.x > 15 THEN 1 ELSE 0 END AS big, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id
--CREATE VIEW jx4 AS SELECT * FROM jx1 WHERE dbl > 30
--CREATE VIEW jx5 AS SELECT aid, dbl * 2 AS q, bid, y FROM jx1
--CREATE VIEW jx6 AS SELECT ja.id AS aid, ja.x - jb.z AS dz, jb.id AS bid, jb.z FROM ja, jb WHERE jb.aid = ja.id
--CREATE VIEW jx7 AS SELECT ja.id AS aid, UPPER(ja.g) AS ug, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id
UPDATE jx1 SET y = 1 WHERE dbl = 40
SELECT id, y FROM jb ORDER BY id
UPDATE jx1 SET y = dbl WHERE bid = 1
SELECT id, y FROM jb ORDER BY id
UPDATE jx1 SET y = y + 1 WHERE dbl > 20 AND bid > 1
SELECT id, y FROM jb ORDER BY id
UPDATE jx1 SET y = 2 WHERE dbl = aid * 20
SELECT id, y FROM jb ORDER BY id
UPDATE jx1 SET y = 3 WHERE NOT dbl > 20
SELECT id, y FROM jb ORDER BY id
UPDATE jx2 SET y = 0 WHERE s = 110
SELECT id, y FROM jb ORDER BY id
UPDATE jx2 SET y = s WHERE bid = 2
SELECT id, y FROM jb ORDER BY id
UPDATE jx3 SET y = 7 WHERE big = 1
SELECT id, y FROM jb ORDER BY id
UPDATE jx3 SET y = big + 40 WHERE bid = 3
SELECT id, y FROM jb ORDER BY id
UPDATE jx4 SET y = 5 WHERE bid = 3
SELECT id, y FROM jb ORDER BY id
UPDATE jx4 SET y = 6 WHERE dbl = 60
SELECT id, y FROM jb ORDER BY id
UPDATE jx5 SET y = 8 WHERE q = 80
SELECT id, y FROM jb ORDER BY id
UPDATE jx6 SET z = 99 WHERE dz = 5
SELECT id, z FROM jb ORDER BY id
UPDATE jx6 SET z = dz WHERE bid = 4
SELECT id, z FROM jb ORDER BY id
UPDATE jx1 SET dbl = 1
UPDATE jx1 SET dbl = 1 WHERE dbl = 40
UPDATE jx1 SET y = 1 WHERE nosuch = dbl
UPDATE jx1 SET y = 1, bid = 7 WHERE dbl = 40
UPDATE jx1 SET aid = aid + 10 WHERE dbl = 80
SELECT id, x FROM ja ORDER BY id
DELETE FROM jx1 WHERE dbl = 40
INSERT INTO jx1 (aid) VALUES (9)
UPDATE jx7 SET y = 1 WHERE ug = '1'
SELECT id, y FROM jb ORDER BY id
SELECT aid, dbl, bid, y FROM jx1 ORDER BY bid
SELECT aid, s, bid FROM jx2 ORDER BY bid
