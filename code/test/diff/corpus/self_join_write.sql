# UPDATE / DELETE that join a table with itself (R52): the second use of the table goes by its alias. All of these changed no row, without an error.
--CREATE TABLE up (id INT PRIMARY KEY, mgr INT, a INT, b INT, g INT)
--CREATE TABLE ot (id INT PRIMARY KEY, a INT, g INT)
--INSERT INTO up VALUES (1,NULL,11,11,1),(2,1,22,22,1),(3,1,30,3,2),(4,2,40,4,2),(5,2,50,5,3)
--INSERT INTO ot VALUES (1,100,1),(2,200,1),(3,300,2)
UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u1.b = u2.a
SELECT * FROM up ORDER BY id
UPDATE up e JOIN up m ON e.mgr = m.id SET e.b = m.a
SELECT * FROM up ORDER BY id
UPDATE up e JOIN up m ON e.mgr = m.id SET e.a = m.a + e.id WHERE e.g = 2
SELECT * FROM up ORDER BY id
UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u2.a = u1.a + 100 WHERE u1.id > 1
SELECT * FROM up ORDER BY id
UPDATE up e LEFT JOIN up m ON e.mgr = m.id SET e.g = COALESCE(m.g, 0) + 10
SELECT * FROM up ORDER BY id
UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 SET u1.g = u2.g + 10, u2.g = 0
SELECT * FROM up ORDER BY id
UPDATE up a, up b SET a.b = b.a WHERE a.id = b.id - 1
SELECT * FROM up ORDER BY id
UPDATE up e JOIN up m ON e.mgr = m.id JOIN ot o ON o.id = m.id SET e.b = o.a
SELECT * FROM up ORDER BY id
UPDATE up u1 JOIN ot o ON u1.id = o.id SET u1.b = o.a + 1
SELECT * FROM up ORDER BY id
UPDATE up u1 JOIN up u2 ON u1.id = u2.id - 1 JOIN up u3 ON u2.id = u3.id - 1 SET u1.b = u3.a
SELECT * FROM up ORDER BY id
UPDATE up JOIN up u2 ON up.id = u2.id - 1 SET up.b = u2.a * 2
SELECT * FROM up ORDER BY id
UPDATE up JOIN up ON up.id = up.id SET up.b = 0
DELETE u1 FROM up u1 JOIN up u2 ON u1.id = u2.id - 1 WHERE u2.id = 4
SELECT * FROM up ORDER BY id
DELETE u2 FROM up u1 JOIN up u2 ON u1.id = u2.id - 1 WHERE u1.id = 1
SELECT * FROM up ORDER BY id
DELETE e FROM up e JOIN up m ON e.mgr = m.id WHERE m.a > 25
SELECT * FROM up ORDER BY id
DELETE FROM up u1 USING up u1 JOIN up u2 ON u1.id = u2.id - 1
# the same statements over two different tables that both have the columns id, v and w (the SET value names the other table by its alias)
--CREATE TABLE t1 (id INT PRIMARY KEY, v INT, w INT)
--CREATE TABLE t2 (id INT PRIMARY KEY, v INT, k INT)
--INSERT INTO t1 VALUES (1,10,100),(2,20,200),(3,30,300),(4,40,400)
--INSERT INTO t2 VALUES (1,111,1),(2,222,1),(3,333,2),(5,555,3)
UPDATE t1 x JOIN t2 y ON x.id = y.id SET x.v = y.v
SELECT * FROM t1 ORDER BY id
UPDATE t1 x JOIN t2 y ON x.id = y.id SET x.w = y.v + x.v WHERE y.k = 1
SELECT * FROM t1 ORDER BY id
UPDATE t1 x LEFT JOIN t2 y ON x.id = y.id SET x.w = y.v
SELECT * FROM t1 ORDER BY id
UPDATE t1 x RIGHT JOIN t2 y ON x.id = y.id SET y.k = x.v
SELECT * FROM t2 ORDER BY id
UPDATE t1, t2 SET t1.w = t2.v, t2.k = t1.v WHERE t1.id = t2.id
SELECT * FROM t1 ORDER BY id
SELECT * FROM t2 ORDER BY id
UPDATE t1 SET v = v + 1, w = v
SELECT * FROM t1 ORDER BY id
DELETE x FROM t1 x JOIN t2 y ON x.id = y.id WHERE y.k = 2
SELECT * FROM t1 ORDER BY id
DELETE x, y FROM t1 x JOIN t2 y ON x.id = y.id
SELECT * FROM t1 ORDER BY id
SELECT * FROM t2 ORDER BY id
DELETE y FROM t1 x RIGHT JOIN t2 y ON x.id = y.id WHERE x.id IS NULL
SELECT * FROM t2 ORDER BY id
