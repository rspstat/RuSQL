# R86 (2026-11-15): ALTER TABLE ... DROP COLUMN takes the column out of the indexes that read it, and an index with no column left goes with it (MySQL; RuSQL left the index on a column
# the table no longer had, and CREATE TABLE ... LIKE then could not copy it). CREATE INDEX on a column the table has not is 1072, DROP INDEX of one that is not there 1091.
# A line is a statement, '--' a setup statement run on both. Run: python vs_mysql.py <engine_server.exe> corpus/index_drop_column_r86.sql --proto --errno
--CREATE TABLE x (id INT PRIMARY KEY, a INT, b INT, c INT)
--INSERT INTO x VALUES (1, 1, 10, 100), (2, 2, 20, 200)
--CREATE INDEX ia ON x (a)
--CREATE INDEX ib ON x (b)
--CREATE INDEX iab ON x (a, b)
--CREATE INDEX ibc ON x (b, c)
SELECT index_name, column_name, seq_in_index FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'x' ORDER BY index_name, seq_in_index
ALTER TABLE x DROP COLUMN b
SELECT index_name, column_name, seq_in_index FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'x' ORDER BY index_name, seq_in_index
SELECT id FROM x WHERE a = 2
SELECT id FROM x WHERE c = 200
CREATE TABLE y LIKE x
SELECT index_name, column_name, seq_in_index FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'y' ORDER BY index_name, seq_in_index
ALTER TABLE x DROP COLUMN a
SELECT index_name, column_name, seq_in_index FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'x' ORDER BY index_name, seq_in_index
SELECT id FROM x WHERE c = 100
CREATE INDEX izz ON x (zz)
CREATE INDEX izz ON x (c, zz)
DROP INDEX ia ON x
DROP INDEX nosuch ON x
DROP INDEX ibc ON x
SELECT index_name, column_name, seq_in_index FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'x' ORDER BY index_name, seq_in_index
