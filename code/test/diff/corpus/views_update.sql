# Writing through views (R71): INSERT / UPDATE / DELETE on a view that maps one table, a view with a WHERE, with expressions, a join, a GROUP BY, a DISTINCT, a view over a view, columns
# the view does not show, WITH CHECK OPTION. A line is a statement, '--' a setup statement run on both.
--CREATE TABLE vb (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(10) NOT NULL, qty INT DEFAULT 5, price INT, grp INT)
--INSERT INTO vb (name, qty, price, grp) VALUES ('a', 1, 10, 1), ('b', 2, 20, 1), ('c', 3, 30, 2), ('d', 4, 40, 2)
--CREATE TABLE vg (id INT PRIMARY KEY, label VARCHAR(10))
--INSERT INTO vg VALUES (1, 'one'), (2, 'two')
--CREATE VIEW v_all AS SELECT id, name, qty FROM vb
--CREATE VIEW v_big AS SELECT id, name, qty FROM vb WHERE qty > 2
--CREATE VIEW v_ren AS SELECT id AS ident, name AS title, price * 2 AS twice FROM vb
--CREATE VIEW v_noname AS SELECT id, qty FROM vb
--CREATE VIEW v_join AS SELECT vb.id, vb.name, vg.label FROM vb JOIN vg ON vg.id = vb.grp
--CREATE VIEW v_agg AS SELECT grp, COUNT(*) AS n, SUM(qty) AS s FROM vb GROUP BY grp
--CREATE VIEW v_dist AS SELECT DISTINCT grp FROM vb
--CREATE VIEW v_over AS SELECT id, name FROM v_big
--CREATE VIEW v_lim AS SELECT id, name FROM vb ORDER BY id LIMIT 2
SELECT * FROM v_all ORDER BY id
INSERT INTO v_all (name, qty) VALUES ('e', 6)
INSERT INTO v_all VALUES (NULL, 'f', 7)
SELECT * FROM vb ORDER BY id
UPDATE v_all SET qty = qty + 100 WHERE name = 'a'
UPDATE v_all SET name = 'bb' WHERE id = 2
DELETE FROM v_all WHERE name = 'f'
SELECT * FROM vb ORDER BY id
# a view with a WHERE: rows that do not fit it are not seen, and a write may take a row out of the view
SELECT * FROM v_big ORDER BY id
UPDATE v_big SET qty = 0 WHERE id = 3
SELECT * FROM v_big ORDER BY id
UPDATE v_big SET qty = 50 WHERE id = 1
DELETE FROM v_big WHERE id = 1
INSERT INTO v_big (name, qty) VALUES ('g', 1)
INSERT INTO v_big (name, qty) VALUES ('h', 9)
SELECT * FROM vb ORDER BY id
# renamed columns and an expression column
SELECT * FROM v_ren ORDER BY ident
UPDATE v_ren SET title = 'ren' WHERE ident = 2
UPDATE v_ren SET twice = 1 WHERE ident = 2
INSERT INTO v_ren (title) VALUES ('t')
SELECT * FROM vb ORDER BY id
# a view that hides a NOT NULL column that has no default
INSERT INTO v_noname (qty) VALUES (1)
INSERT INTO v_noname (id, qty) VALUES (500, 1)
# joins, aggregates, DISTINCT, LIMIT: what MySQL allows to write through
# (not compared: UPDATE and INSERT through a join view, which MySQL takes when they name the columns of one table)
DELETE FROM v_join WHERE id = 1
UPDATE v_agg SET n = 1
DELETE FROM v_agg
INSERT INTO v_agg (grp) VALUES (9)
UPDATE v_dist SET grp = 0
DELETE FROM v_dist
UPDATE v_lim SET name = 'lm' WHERE id = 1
DELETE FROM v_lim WHERE id = 1
SELECT * FROM vb ORDER BY id
# a view over a view
SELECT * FROM v_over ORDER BY id
UPDATE v_over SET name = 'ov' WHERE id = 4
INSERT INTO v_over (name) VALUES ('w')
SELECT * FROM vb ORDER BY id
# WITH CHECK OPTION
--CREATE VIEW v_chk AS SELECT id, name, qty FROM vb WHERE qty < 10 WITH CHECK OPTION
--CREATE VIEW v_loc AS SELECT id, name, qty FROM vb WHERE qty < 10 WITH LOCAL CHECK OPTION
--CREATE VIEW v_cas AS SELECT id, name, qty FROM v_chk WHERE qty > 0 WITH CASCADED CHECK OPTION
INSERT INTO v_chk (name, qty) VALUES ('ok', 3)
INSERT INTO v_chk (name, qty) VALUES ('bad', 30)
UPDATE v_chk SET qty = 30 WHERE name = 'ok'
UPDATE v_chk SET qty = 4 WHERE name = 'ok'
INSERT INTO v_cas (name, qty) VALUES ('c0', 0)
INSERT INTO v_cas (name, qty) VALUES ('c1', 1)
SELECT id, name, qty FROM vb WHERE name IN ('ok', 'bad', 'c0', 'c1') ORDER BY id
# views and changes of the table below
--ALTER TABLE vb ADD COLUMN extra INT DEFAULT 9
SELECT * FROM v_all ORDER BY id
CREATE OR REPLACE VIEW v_all AS SELECT id, name, qty, extra FROM vb
SELECT * FROM v_all ORDER BY id LIMIT 3
ALTER VIEW v_all AS SELECT id, name FROM vb
SELECT * FROM v_all ORDER BY id LIMIT 3
DROP VIEW v_all
SELECT * FROM v_all
DROP VIEW IF EXISTS v_all
