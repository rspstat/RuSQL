# the column types the MySQL protocol reports (run with --proto --types): numbers, dates and text must come out as the same kinds as from a real MySQL
--CREATE TABLE ty (id INT PRIMARY KEY AUTO_INCREMENT, t TINYINT, s SMALLINT, b BIGINT, u INT UNSIGNED, f FLOAT, d DOUBLE, dec1 DECIMAL(10,2), dec0 DECIMAL(8,0), vc VARCHAR(20), tx TEXT, dt DATE, ts DATETIME, tm TIME, yr YEAR, flag BOOLEAN, js JSON)
--INSERT INTO ty VALUES (1, 1, 2, 3, 4, 1.5, 2.25, 10.50, 7, 'a', 'text', '2024-01-31', '2024-01-31 10:20:30', '10:20:30', 2024, 1, '{"a": 1}'), (2, -1, -2, -3, 4000000000, -1.5, -2.25, -10.50, -7, 'b', 'more', '2023-12-25', '2023-12-25 00:00:00', '-01:30:00', 1999, 0, '[1, 2]'), (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)
--CREATE TABLE ty2 (id INT PRIMARY KEY, label VARCHAR(10))
--INSERT INTO ty2 VALUES (1, 'one'), (2, 'two')
SELECT * FROM ty ORDER BY id
SELECT id, t, s, b, u FROM ty ORDER BY id
SELECT f, d, dec1, dec0 FROM ty ORDER BY id
SELECT vc, tx, js FROM ty ORDER BY id
SELECT dt, ts, tm, yr, flag FROM ty ORDER BY id
SELECT COUNT(*), COUNT(t), COUNT(DISTINCT vc) FROM ty
SELECT SUM(id), SUM(t), SUM(dec1), SUM(d), SUM(f) FROM ty
SELECT AVG(id), AVG(dec1), AVG(d) FROM ty
SELECT MIN(id), MAX(id), MIN(dec1), MAX(dt), MIN(vc), MAX(ts) FROM ty
SELECT id, id * 2, id + 0.5, id / 2, -id FROM ty ORDER BY id
SELECT ROUND(d, 1), ABS(t), LENGTH(vc), CHAR_LENGTH(vc) FROM ty ORDER BY id
SELECT CONCAT(vc, 'x'), UPPER(vc), SUBSTRING(tx, 1, 2) FROM ty ORDER BY id
SELECT vc AS name, id AS pk, dec1 AS price FROM ty ORDER BY id
SELECT x.id, x.dec1, y.label FROM ty x JOIN ty2 y ON x.id = y.id ORDER BY x.id
SELECT x.id, y.label FROM ty x LEFT JOIN ty2 y ON x.id = y.id ORDER BY x.id
SELECT ty.*, ty2.label FROM ty JOIN ty2 ON ty.id = ty2.id ORDER BY ty.id
SELECT * FROM (SELECT id, dec1, dt FROM ty) q ORDER BY id
SELECT t, COUNT(*), SUM(id) FROM ty GROUP BY t ORDER BY t
SELECT COUNT(*) OVER (), ROW_NUMBER() OVER (ORDER BY id), SUM(id) OVER (ORDER BY id), LAG(dec1) OVER (ORDER BY id), PERCENT_RANK() OVER (ORDER BY id) FROM ty ORDER BY id
SELECT 1, 1.5, 'a', NULL
SELECT 1 + 1, 2 * 3.5, 10 / 4
SELECT id FROM ty UNION SELECT id FROM ty2 ORDER BY id
--CREATE VIEW ty_v AS SELECT id, dt, dec1, vc FROM ty
SELECT * FROM ty_v ORDER BY id
SELECT table_name, table_rows, table_schema FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = 'ty'
SELECT column_name, ordinal_position, column_default FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'ty2' ORDER BY ordinal_position
SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE()
SELECT @@autocommit, DATABASE(), VERSION()
SELECT LAST_INSERT_ID(), ROW_COUNT(), FOUND_ROWS()
SELECT id FROM ty2 UNION SELECT label FROM ty2
SELECT label, id FROM ty2 UNION ALL SELECT vc, id FROM ty
