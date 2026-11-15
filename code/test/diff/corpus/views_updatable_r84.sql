# R84: information_schema.views.IS_UPDATABLE for views of many shapes (RECOMMAND R83 leftover 4: it was an approximation). Run: python vs_mysql.py <engine_server.exe> corpus/views_updatable_r84.sql --proto --errno
--CREATE TABLE ua (id INT PRIMARY KEY, k INT, name VARCHAR(10))
--CREATE TABLE ub (id INT PRIMARY KEY, ua_id INT, qty INT)
--INSERT INTO ua VALUES (1, 10, 'a'), (2, 20, 'b')
--INSERT INTO ub VALUES (1, 1, 5), (2, 1, 7), (3, 2, 9)
--CREATE VIEW vu84_plain AS SELECT id, k FROM ua
--CREATE VIEW vu84_where AS SELECT id, k FROM ua WHERE k > 5
--CREATE VIEW vu84_expr AS SELECT id, k + 1 AS k1 FROM ua
--CREATE VIEW vu84_const AS SELECT 1 AS one
--CREATE VIEW vu84_distinct AS SELECT DISTINCT k FROM ua
--CREATE VIEW vu84_group AS SELECT k, COUNT(*) AS n FROM ua GROUP BY k
--CREATE VIEW vu84_agg AS SELECT COUNT(*) AS n FROM ua
--CREATE VIEW vu84_having AS SELECT k FROM ua GROUP BY k HAVING COUNT(*) > 0
--CREATE VIEW vu84_union AS SELECT id FROM ua UNION SELECT id FROM ub
--CREATE VIEW vu84_unionall AS SELECT id FROM ua UNION ALL SELECT id FROM ub
--CREATE VIEW vu84_join AS SELECT ua.id, ub.qty FROM ua JOIN ub ON ub.ua_id = ua.id
--CREATE VIEW vu84_joinagg AS SELECT ua.id, COUNT(ub.id) AS n FROM ua JOIN ub ON ub.ua_id = ua.id GROUP BY ua.id
--CREATE VIEW vu84_leftjoin AS SELECT ua.id, ub.qty FROM ua LEFT JOIN ub ON ub.ua_id = ua.id
--CREATE VIEW vu84_subsel AS SELECT id, (SELECT COUNT(*) FROM ub WHERE ub.ua_id = ua.id) AS n FROM ua
--CREATE VIEW vu84_subwhere AS SELECT id, k FROM ua WHERE id IN (SELECT ua_id FROM ub)
--CREATE VIEW vu84_limit AS SELECT id, k FROM ua LIMIT 1
--CREATE VIEW vu84_order AS SELECT id, k FROM ua ORDER BY k DESC
--CREATE VIEW vu84_ofview AS SELECT id FROM vu84_plain
--CREATE VIEW vu84_ofagg AS SELECT k FROM vu84_group
--CREATE VIEW vu84_win AS SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM ua
--CREATE VIEW vu84_func AS SELECT id, UPPER(name) AS up FROM ua
--CREATE VIEW vu84_selfjoin AS SELECT a.id, b.k FROM ua a JOIN ua b ON a.id = b.id
--CREATE VIEW vu84_cross AS SELECT ua.id, ub.qty FROM ua, ub
--CREATE ALGORITHM = TEMPTABLE VIEW vu84_temptable AS SELECT id, k FROM ua
--CREATE VIEW vu84_derived AS SELECT d.id FROM (SELECT id FROM ua) d
--CREATE VIEW vu84_chk AS SELECT id, k FROM ua WHERE k > 5 WITH CHECK OPTION
--CREATE ALGORITHM = MERGE VIEW vu84_merge AS SELECT id, k FROM ua
--CREATE VIEW vu84_exists AS SELECT id FROM ua WHERE EXISTS (SELECT 1 FROM ub WHERE ub.ua_id = ua.id)
--CREATE VIEW vu84_distinctrow AS SELECT DISTINCT id, k FROM ua
--CREATE VIEW vu84_star AS SELECT * FROM ua
--CREATE VIEW vu84_joinexpr AS SELECT ua.id, ub.qty + 1 AS q FROM ua JOIN ub ON ub.ua_id = ua.id
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_plain'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_where'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_expr'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_const'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_distinct'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_group'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_agg'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_having'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_union'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_unionall'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_join'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_joinagg'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_leftjoin'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_subsel'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_subwhere'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_limit'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_order'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_ofview'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_ofagg'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_win'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_func'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_selfjoin'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_cross'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_temptable'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_derived'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_chk'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_merge'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_exists'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_distinctrow'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_star'
SELECT table_name, is_updatable FROM information_schema.views WHERE table_name = 'vu84_joinexpr'
UPDATE vu84_plain SET k = k WHERE id = 1
UPDATE vu84_expr SET k1 = 1 WHERE id = 1
UPDATE vu84_distinct SET k = 1
UPDATE vu84_group SET n = 1
UPDATE vu84_union SET id = 1
UPDATE vu84_join SET qty = 5 WHERE id = 1
UPDATE vu84_joinagg SET n = 1
UPDATE vu84_leftjoin SET qty = 5 WHERE id = 1
UPDATE vu84_subsel SET n = 1
UPDATE vu84_limit SET k = 11 WHERE id = 1
UPDATE vu84_ofagg SET k = 1
UPDATE vu84_cross SET qty = 6 WHERE id = 1
INSERT INTO vu84_plain (id, k) VALUES (30, 3)
INSERT INTO vu84_join (id, qty) VALUES (31, 3)
INSERT INTO vu84_expr (id, k1) VALUES (32, 3)
DELETE FROM vu84_plain WHERE id = 30
DELETE FROM vu84_join WHERE id = 1
DELETE FROM vu84_distinct
DELETE FROM vu84_group
# writes through a view that has a column of an expression, or of a subquery
INSERT INTO vu84_expr (id) VALUES (33)
INSERT INTO vu84_expr (id, k1) VALUES (34, 1)
INSERT INTO vu84_expr VALUES (35, 1)
SELECT id, k FROM ua ORDER BY id
UPDATE vu84_expr SET id = 40 WHERE id = 33
UPDATE vu84_expr SET k1 = 2
UPDATE vu84_expr SET id = 41 WHERE k1 = 11
DELETE FROM vu84_expr WHERE k1 = 21
SELECT id, k FROM ua ORDER BY id
UPDATE vu84_temptable SET k = 11 WHERE id = 1
DELETE FROM vu84_temptable WHERE id = 99
INSERT INTO vu84_temptable (id, k) VALUES (9, 9)
UPDATE vu84_derived SET id = 5 WHERE id = 99
DELETE FROM vu84_derived WHERE id = 99
INSERT INTO vu84_derived (id) VALUES (50)
SELECT id, k FROM ua ORDER BY id
INSERT INTO vu84_subsel (id) VALUES (50)
INSERT INTO vu84_subsel (id, n) VALUES (51, 1)
UPDATE vu84_subsel SET id = 60 WHERE id = 50
UPDATE vu84_subsel SET id = 61 WHERE n = 2
UPDATE vu84_subsel SET n = 3
DELETE FROM vu84_subsel WHERE n = 2
DELETE FROM vu84_subsel WHERE id = 60
SELECT id, k FROM ua ORDER BY id
INSERT INTO vu84_func (id) VALUES (70)
UPDATE vu84_func SET up = 'x'
INSERT INTO vu84_leftjoin (id, qty) VALUES (80, 1)
UPDATE vu84_leftjoin SET qty = 1
DELETE FROM vu84_leftjoin WHERE id = 1
INSERT INTO vu84_group (k, n) VALUES (1, 1)
INSERT INTO vu84_joinagg (id, n) VALUES (1, 1)
INSERT INTO vu84_union (id) VALUES (1)
INSERT INTO vu84_distinct (k) VALUES (1)
DELETE FROM vu84_union
DELETE FROM vu84_joinagg
DELETE FROM vu84_leftjoin
DELETE FROM vu84_cross WHERE id = 1
UPDATE vu84_ofview SET id = 1 WHERE id = 99
