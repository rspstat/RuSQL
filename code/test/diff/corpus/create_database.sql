# CREATE DATABASE does not move the session into the new database (R79; it did): the next CREATE TABLE is still made in the database the session is in, a table of the new one
# is written db.table or after a USE. CREATE TABLE ... AS SELECT reports the rows it copied as the affected rows of its OK packet (it said 0). Run with --proto (OK packets).
--DROP DATABASE IF EXISTS other79
SELECT DATABASE()
CREATE DATABASE other79
SELECT DATABASE()
CREATE TABLE here (a INT)
SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'v' AND table_name = 'here'
SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'other79' AND table_name = 'here'
CREATE TABLE other79.there (a INT)
SELECT table_schema, table_name FROM information_schema.tables WHERE table_name IN ('here', 'there') ORDER BY table_schema
SHOW TABLES
SHOW TABLES FROM other79
USE other79
SELECT DATABASE()
SHOW TABLES
USE v
CREATE DATABASE IF NOT EXISTS other79
SELECT DATABASE()
CREATE DATABASE other79
DROP DATABASE other79
SELECT DATABASE()
CREATE TABLE c1 AS SELECT id, s FROM t
SELECT ROW_COUNT()
CREATE TABLE c2 AS SELECT id FROM t WHERE id > 100
SELECT ROW_COUNT()
CREATE TABLE c3 AS SELECT id FROM t UNION SELECT k FROM u
CREATE TABLE c4 (x BIGINT) AS SELECT id FROM t WHERE id < 4
CREATE TABLE IF NOT EXISTS c1 AS SELECT id FROM t
SELECT id, s FROM c1 ORDER BY id LIMIT 3
SELECT COUNT(*) FROM c3
SELECT ROW_COUNT()
CREATE TABLE c5 AS SELECT 1 AS one
INSERT INTO c5 SELECT id FROM t WHERE id < 4
SELECT ROW_COUNT()
DROP DATABASE IF EXISTS o2
CREATE DATABASE o2
CREATE TABLE o2.a (x INT)
CREATE TABLE o2.b (x INT)
CREATE VIEW o2.vw AS SELECT 1 AS one
DROP DATABASE o2
DROP DATABASE IF EXISTS o2
DROP DATABASE o2
CREATE DATABASE o2
DROP DATABASE o2
SELECT ROW_COUNT()
