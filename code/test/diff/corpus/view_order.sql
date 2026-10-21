# UPDATE / DELETE through a view with ORDER BY and LIMIT (R45): the rows are picked, in the order asked for, from the table the view reads.
--CREATE TABLE vo_t (id INT PRIMARY KEY, grp INT, u INT UNIQUE, v INT)
--INSERT INTO vo_t VALUES (1,1,1,0),(2,1,2,0),(3,2,3,0),(4,2,4,0),(5,3,5,0)
--CREATE VIEW vo_v AS SELECT id, grp, u, v FROM vo_t WHERE grp < 3
UPDATE vo_v SET v = 1 ORDER BY id DESC LIMIT 2
SELECT * FROM vo_t ORDER BY id
UPDATE vo_v SET u = u + 10 ORDER BY u DESC
SELECT * FROM vo_t ORDER BY id
UPDATE vo_v SET u = u - 9
SELECT * FROM vo_t ORDER BY id
UPDATE vo_v SET v = 9 WHERE grp = 2 ORDER BY id LIMIT 1
SELECT * FROM vo_t ORDER BY id
UPDATE vo_v SET v = 7 LIMIT 3
SELECT * FROM vo_t ORDER BY id
UPDATE vo_v SET u = u + 1 ORDER BY id DESC LIMIT 2
SELECT * FROM vo_t ORDER BY id
DELETE FROM vo_v ORDER BY id DESC LIMIT 1
SELECT * FROM vo_t ORDER BY id
DELETE FROM vo_v WHERE grp = 1 ORDER BY id LIMIT 1
SELECT * FROM vo_t ORDER BY id
DELETE FROM vo_v LIMIT 5
SELECT * FROM vo_t ORDER BY id
