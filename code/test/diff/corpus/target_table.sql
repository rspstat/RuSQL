# The table an UPDATE or a DELETE changes cannot be read by a subquery of it (R50): MySQL 1093 "You can't specify target table 'x' for update in FROM clause". A derived table is made first,
# so it may read the table; so may a join of the table with itself, and an INSERT ... SELECT. The statements that fail leave the rows as they were.
--CREATE TABLE tt (id INT PRIMARY KEY, v INT, g INT)
--INSERT INTO tt VALUES (1,10,1),(2,20,1),(3,30,2),(4,NULL,2)
--CREATE TABLE tu (id INT PRIMARY KEY, v INT)
--INSERT INTO tu VALUES (1,5),(2,50)
UPDATE tt SET v = 1 WHERE id IN (SELECT id FROM tt WHERE v > 5)
DELETE FROM tt WHERE id IN (SELECT id FROM tt WHERE v > 15)
UPDATE tt SET v = (SELECT MAX(v) FROM tt) WHERE id = 1
UPDATE tt SET v = v + 1 WHERE EXISTS (SELECT 1 FROM tt q WHERE q.id = tt.id)
UPDATE tt SET v = 5 WHERE v > (SELECT AVG(v) FROM tt)
DELETE FROM tt WHERE v > (SELECT AVG(v) FROM tt)
DELETE FROM tt WHERE EXISTS (SELECT 1 FROM tt x WHERE x.v > 100)
UPDATE tt SET v = 0 WHERE id IN (SELECT id FROM tu JOIN tt ON tu.id = tt.id)
UPDATE tt SET v = 0 WHERE id IN (SELECT id FROM tt UNION SELECT id FROM tu)
UPDATE tt SET v = 1 WHERE id IN (SELECT id FROM (SELECT id FROM tt WHERE v > 5) x)
UPDATE tt SET v = (SELECT MAX(x.v) FROM (SELECT v FROM tt) x) WHERE id = 2
UPDATE tt SET v = (SELECT MAX(v) FROM tu) WHERE id = 3
UPDATE tt SET v = 7 WHERE id IN (SELECT id FROM tu WHERE v > 10)
DELETE FROM tt WHERE id IN (SELECT id FROM tu WHERE v > 100)
UPDATE tt a JOIN tt b ON a.id = b.id SET a.v = b.v + 1 WHERE a.id = 1
UPDATE tt JOIN tu ON tt.id = tu.id SET tt.v = tu.v WHERE tt.id = 1
UPDATE tt SET v = 3 WHERE id IN (SELECT id FROM tt)
UPDATE tt SET g = (SELECT COUNT(*) FROM tt WHERE g = 1) WHERE id = 1
INSERT INTO tt (id, v, g) SELECT id + 10, v, g FROM tt
INSERT INTO tt (id, v) SELECT id + 20, (SELECT MAX(v) FROM tt) FROM tu
INSERT INTO tt (id, v) VALUES (30, (SELECT MAX(v) FROM tt))
INSERT INTO tt (id, v) VALUES (31, (SELECT MAX(v) FROM tu))
INSERT INTO tt (id, v) VALUES (1, 1) ON DUPLICATE KEY UPDATE v = (SELECT MAX(v) FROM tt)
INSERT INTO tt (id, v) VALUES (1, 1) ON DUPLICATE KEY UPDATE v = (SELECT MAX(v) FROM tu)
REPLACE INTO tt (id, v) VALUES (32, (SELECT MAX(v) FROM tt))
UPDATE tt a SET v = 1 WHERE id IN (SELECT id FROM tt)
DELETE FROM tt WHERE id IN (SELECT id FROM tt b WHERE b.v > 5)
SELECT id, v, g FROM tt ORDER BY id
