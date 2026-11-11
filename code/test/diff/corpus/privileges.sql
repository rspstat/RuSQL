# Users, privileges and roles (R77), run with --proto --errno: CREATE / ALTER / DROP USER, GRANT and REVOKE (a table, a database, everything, WITH GRANT OPTION), SHOW GRANTS, what a user may
# and may not do (error 1142 / 1227 / 1044), roles (CREATE ROLE, GRANT role, SET ROLE, CURRENT_ROLE(), default roles). A line is a statement, '--' a setup statement run on both, '@name ' a
# statement run by that user on a connection of his own (password 'pw'). The tables t and u of vs_mysql.py are in the database v.
--DROP USER IF EXISTS 'alice'@'%'
--DROP USER IF EXISTS 'bob'@'%'
--DROP ROLE IF EXISTS 'reader'
--DROP ROLE IF EXISTS 'writer'
--DROP ROLE IF EXISTS 'auditor'
--DROP ROLE IF EXISTS 'a1'
--DROP ROLE IF EXISTS 'a2'
# creating and dropping users
CREATE USER 'alice'@'%' IDENTIFIED BY 'pw'
CREATE USER 'alice'@'%' IDENTIFIED BY 'pw'
CREATE USER IF NOT EXISTS 'alice'@'%' IDENTIFIED BY 'pw'
SHOW GRANTS FOR 'alice'@'%'
SHOW GRANTS FOR 'nobody'@'%'
DROP USER 'nobody'@'%'
DROP USER IF EXISTS 'nobody'@'%'
ALTER USER 'nobody'@'%' IDENTIFIED BY 'pw2'
ALTER USER IF EXISTS 'nobody'@'%' IDENTIFIED BY 'pw2'
ALTER USER 'alice'@'%' IDENTIFIED BY 'pw2'
ALTER USER 'alice'@'%' IDENTIFIED BY 'pw'
SET PASSWORD FOR 'alice'@'%' = 'pw'
RENAME USER 'bob'@'%' TO 'rob'@'%'
# a user with no privilege at all cannot even use the database
@bob SELECT 1
# one table
GRANT SELECT ON v.t TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
@alice SELECT id FROM t ORDER BY id LIMIT 3
@alice SELECT COUNT(*) FROM v.t
@alice SELECT * FROM u
@alice INSERT INTO t (id, i) VALUES (50, 1)
@alice UPDATE t SET i = 0
@alice DELETE FROM t
@alice CREATE TABLE z (a INT)
@alice DROP TABLE t
@alice SELECT 1
@alice SELECT CURRENT_USER()
@alice SHOW GRANTS
@alice SHOW TABLES
@alice DESCRIBE u
@alice SHOW CREATE TABLE u
@alice GRANT SELECT ON v.t TO 'bob'@'%'
# more privileges, and taking them away
GRANT INSERT, UPDATE ON v.t TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
@alice INSERT INTO t (id, i) VALUES (50, 1)
@alice UPDATE t SET i = i + 1 WHERE id = 50
@alice DELETE FROM t WHERE id = 50
@alice INSERT INTO t (id, i) SELECT id + 100, i FROM t
@alice SELECT COUNT(*) FROM t
REVOKE INSERT ON v.t FROM 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
@alice INSERT INTO t (id, i) VALUES (51, 1)
REVOKE SELECT, UPDATE ON v.t FROM 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
REVOKE SELECT ON v.t FROM 'alice'@'%'
# a database
GRANT ALL ON v.* TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
# (MySQL gives a connection the privileges of a database when it connects or says USE: the line below is that)
@alice USE v
@alice SELECT COUNT(*) FROM t
@alice CREATE TABLE z (a INT)
@alice DROP TABLE z
@alice CREATE USER 'bob'@'%'
@alice GRANT SELECT ON v.t TO 'bob'@'%'
GRANT ALL ON v.* TO 'alice'@'%' WITH GRANT OPTION
SHOW GRANTS FOR 'alice'@'%'
GRANT SELECT ON *.* TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
# the errors of GRANT and REVOKE
GRANT SELECT ON v.t TO 'nobody'@'%'
GRANT FOO ON v.t TO 'alice'@'%'
GRANT SELECT ON v.nosuch TO 'alice'@'%'
REVOKE SELECT ON v.t FROM 'nobody'@'%'
REVOKE SELECT ON v.t FROM 'alice'@'%'
# columns
GRANT SELECT (i) ON v.t TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
@alice USE v
@alice SELECT i FROM t WHERE id = 1
@alice SELECT id FROM t
@alice SELECT * FROM t
REVOKE SELECT (i) ON v.t FROM 'alice'@'%'
# roles
CREATE ROLE 'reader'
CREATE ROLE 'reader'
CREATE ROLE IF NOT EXISTS 'reader'
CREATE ROLE 'writer', 'auditor'
GRANT SELECT ON v.* TO 'reader'
GRANT INSERT, UPDATE, DELETE ON v.* TO 'writer'
SHOW GRANTS FOR 'reader'
SHOW GRANTS FOR 'reader'@'%'
GRANT 'reader' TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%' USING 'reader'
@alice SELECT COUNT(*) FROM t
@alice SELECT CURRENT_ROLE()
@alice SET ROLE 'reader'
@alice SELECT CURRENT_ROLE()
@alice SELECT COUNT(*) FROM t
@alice SET ROLE NONE
@alice SELECT CURRENT_ROLE()
@alice SELECT COUNT(*) FROM t
@alice SET ROLE ALL
@alice SELECT CURRENT_ROLE()
@alice SELECT COUNT(*) FROM t
@alice SET ROLE 'writer'
SET DEFAULT ROLE 'reader' TO 'alice'@'%'
GRANT 'writer', 'auditor' TO 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
@alice SET ROLE 'writer'
@alice SELECT CURRENT_ROLE()
@alice INSERT INTO t (id, i) VALUES (60, 1)
@alice SELECT COUNT(*) FROM t
@alice SET ROLE ALL EXCEPT 'writer'
@alice SELECT CURRENT_ROLE()
REVOKE 'writer' FROM 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
GRANT 'nosuchrole' TO 'alice'@'%'
REVOKE 'auditor' FROM 'bob'@'%'
GRANT 'reader' TO 'reader'
CREATE ROLE 'a1'
CREATE ROLE 'a2'
GRANT 'a1' TO 'a2'
GRANT 'a2' TO 'a1'
DROP ROLE 'a1', 'a2'
DROP ROLE 'a1'
DROP ROLE IF EXISTS 'a1'
DROP USER 'alice'@'%'
SHOW GRANTS FOR 'alice'@'%'
SELECT CURRENT_USER()
# --- what a user with one privilege may do (every scenario has a user of its own: the privileges of a database are read when the user connects) ---
--CREATE DATABASE IF NOT EXISTS w
--CREATE TABLE IF NOT EXISTS w.x (id INT PRIMARY KEY)
--USE v
--DROP VIEW IF EXISTS vw
--CREATE VIEW vw AS SELECT id, i FROM t
--DROP PROCEDURE IF EXISTS p
--CREATE PROCEDURE p() SELECT 1
--DROP USER IF EXISTS 's1'@'%'
--DROP USER IF EXISTS 's2'@'%'
--DROP USER IF EXISTS 's3'@'%'
--DROP USER IF EXISTS 's4'@'%'
--DROP USER IF EXISTS 's5'@'%'
--DROP USER IF EXISTS 's6'@'%'
--DROP USER IF EXISTS 's7'@'%'
--DROP USER IF EXISTS 's7b'@'%'
--DROP USER IF EXISTS 's8'@'%'
--CREATE USER 's1'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's2'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's3'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's4'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's5'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's6'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's7'@'%' IDENTIFIED BY 'pw'
--CREATE USER 's8'@'%' IDENTIFIED BY 'pw'
# INSERT only
--GRANT INSERT ON v.t TO 's1'@'%'
@s1 INSERT INTO t (id, i) VALUES (70, 1)
@s1 INSERT INTO t (id, i) VALUES (1, 1) ON DUPLICATE KEY UPDATE i = 2
@s1 REPLACE INTO t (id, i) VALUES (1, 1)
@s1 INSERT IGNORE INTO t (id, i) VALUES (1, 1)
@s1 INSERT INTO t (id, i) SELECT id + 100, i FROM t
@s1 SELECT 1
@s1 SELECT * FROM t
# UPDATE only
--GRANT UPDATE ON v.t TO 's2'@'%'
@s2 UPDATE t SET i = 5
@s2 UPDATE t SET i = 5 WHERE id = 1
@s2 UPDATE t SET i = i + 1
@s2 UPDATE t SET i = id
@s2 UPDATE t, u SET t.i = 1 WHERE t.id = u.id
@s2 UPDATE t SET id = id + 100 WHERE 1 = 1
# DELETE only
--GRANT DELETE ON v.t TO 's3'@'%'
@s3 DELETE FROM t WHERE id = 999
@s3 DELETE FROM t WHERE 1 = 0
@s3 TRUNCATE TABLE t
# SELECT only, on one table
--GRANT SELECT ON v.t TO 's4'@'%'
@s4 SELECT * FROM t WHERE id < 3
@s4 SELECT * FROM t, u
@s4 SELECT * FROM t WHERE id IN (SELECT id FROM u)
@s4 SELECT (SELECT COUNT(*) FROM u) FROM t
@s4 SELECT * FROM vw
@s4 WITH c AS (SELECT * FROM t) SELECT COUNT(*) FROM c
@s4 SELECT id FROM t UNION SELECT id FROM u
@s4 EXPLAIN SELECT * FROM u
@s4 DESCRIBE t
@s4 DESCRIBE u
@s4 SHOW COLUMNS FROM u
@s4 SHOW INDEX FROM u
@s4 SHOW CREATE TABLE t
@s4 SHOW TABLES FROM v
@s4 SHOW TABLES FROM w
@s4 USE v
@s4 USE w
@s4 SELECT * FROM w.x
@s4 SELECT * FROM t FOR UPDATE
@s4 LOCK TABLES t READ
@s4 UNLOCK TABLES
@s4 ANALYZE TABLE t
@s4 CHECK TABLE t
@s4 OPTIMIZE TABLE t
@s4 CREATE INDEX ix ON t (i)
@s4 ALTER TABLE t ADD COLUMN k INT
@s4 CREATE TABLE n (a INT)
@s4 CREATE DATABASE z
@s4 DROP DATABASE w
@s4 CALL p()
@s4 CREATE PROCEDURE q() SELECT 2
@s4 CREATE VIEW v2 AS SELECT * FROM t
@s4 SHOW CREATE VIEW vw
@s4 PREPARE s1 FROM 'SELECT * FROM u'
@s4 EXECUTE s1
@s4 TRUNCATE TABLE t
@s4 RENAME TABLE t TO t2
@s4 DROP VIEW vw
# everything on one database
--GRANT ALL ON v.* TO 's5'@'%'
@s5 CREATE TABLE n (a INT)
@s5 DROP TABLE n
@s5 CREATE VIEW v2 AS SELECT * FROM t
@s5 DROP VIEW v2
@s5 CREATE DATABASE z
@s5 CALL p()
@s5 CREATE PROCEDURE q() SELECT 2
@s5 DROP PROCEDURE q
@s5 CREATE INDEX ix ON t (i)
@s5 DROP INDEX ix ON t
@s5 ALTER TABLE t ADD COLUMN k INT
@s5 ALTER TABLE t DROP COLUMN k
@s5 RENAME TABLE u TO u2
@s5 RENAME TABLE u2 TO u
@s5 LOCK TABLES t READ
@s5 UNLOCK TABLES
@s5 SHOW TABLES FROM v
@s5 USE w
@s5 SHOW GRANTS
@s5 GRANT SELECT ON v.t TO 's6'@'%'
@s5 SELECT * FROM w.x
@s5 CREATE TABLE cc AS SELECT * FROM t WHERE 1 = 0
@s5 CREATE TABLE cl LIKE t
@s5 DROP TABLE cc
@s5 DROP TABLE cl
# columns
--GRANT SELECT (i) ON v.t TO 's6'@'%'
--GRANT UPDATE (k) ON v.u TO 's6'@'%'
@s6 SELECT i FROM t
@s6 SELECT id FROM t
@s6 SELECT i FROM t WHERE id = 1
@s6 SELECT i FROM t ORDER BY i
@s6 SELECT COUNT(*) FROM t
@s6 SELECT COUNT(i) FROM t
@s6 SELECT * FROM t
@s6 SELECT i, 1 FROM t
@s6 UPDATE u SET k = 1
@s6 UPDATE u SET k = 1 WHERE id = 1
@s6 UPDATE u SET id = 1
@s6 SHOW GRANTS
@s6 SELECT 1 FROM t
@s6 SELECT i FROM t, u
# roles that hold roles
--DROP ROLE IF EXISTS 'ra'
--DROP ROLE IF EXISTS 'rb'
--CREATE ROLE 'ra', 'rb'
--GRANT SELECT ON v.t TO 'ra'
--GRANT SELECT ON v.u TO 'rb'
--GRANT 'rb' TO 'ra'
--GRANT 'ra' TO 's7'@'%' WITH ADMIN OPTION
--GRANT SHOW VIEW ON v.* TO 's7'@'%'
@s7 SELECT CURRENT_ROLE()
@s7 SELECT * FROM t
@s7 SET ROLE ALL
@s7 SELECT CURRENT_ROLE()
@s7 SELECT * FROM t WHERE id < 2
@s7 SELECT * FROM u WHERE id < 2
@s7 SET ROLE 'ra'
@s7 SELECT CURRENT_ROLE()
@s7 SELECT * FROM u WHERE id < 2
@s7 SET ROLE NONE
@s7 SELECT CURRENT_ROLE()
@s7 SET ROLE 'rb'
@s7 SET ROLE 'ra', 'rb'
@s7 SET ROLE DEFAULT
@s7 SELECT CURRENT_ROLE()
@s7 GRANT 'ra' TO 's8'@'%'
@s7 GRANT 'rb' TO 's8'@'%'
@s7 REVOKE 'ra' FROM 's8'@'%'
@s7 SHOW GRANTS
@s7 SHOW GRANTS FOR CURRENT_USER()
@s7 SHOW GRANTS FOR 's7'@'%' USING 'ra'
# what a user without administrative privileges is refused
--GRANT SELECT ON v.t TO 's8'@'%'
@s8 GRANT SELECT ON v.t TO 's4'@'%'
@s8 GRANT SELECT ON v.* TO 's4'@'%'
@s8 GRANT SELECT ON *.* TO 's4'@'%'
@s8 REVOKE SELECT ON v.t FROM 's4'@'%'
@s8 REVOKE SELECT ON v.* FROM 's4'@'%'
@s8 REVOKE SELECT ON *.* FROM 's4'@'%'
@s8 CREATE USER 's9'@'%'
@s8 ALTER USER 's4'@'%' IDENTIFIED BY 'pw'
@s8 ALTER USER 's8'@'%' IDENTIFIED BY 'pw'
@s8 SET PASSWORD = 'pw'
@s8 SET PASSWORD FOR 's4'@'%' = 'pw'
@s8 RENAME USER 's4'@'%' TO 's9'@'%'
@s8 DROP USER 's4'@'%'
@s8 CREATE ROLE 'r9'
@s8 DROP ROLE 'ra'
@s8 GRANT 'ra' TO 's4'@'%'
@s8 REVOKE 'ra' FROM 's4'@'%'
@s8 SHOW GRANTS FOR 's4'@'%'
@s8 SHOW GRANTS FOR 's8'@'%'
@s8 SELECT CURRENT_USER(), USER(), CURRENT_ROLE()
@s8 SET ROLE 'ra'
@s8 SET GLOBAL slow_query_log = 'OFF'
@s8 SET @@global.long_query_time = 5
@s8 SET @x = 1
@s8 SET @global = 'a global thing'
@s8 SET SESSION sql_mode = ''
# GRANT and REVOKE by root at the edges
REVOKE SELECT ON v.t FROM 's9'@'%'
GRANT SELECT ON v.* TO 's9'@'%'
GRANT SELECT ON nosuchdb.* TO 's1'@'%'
GRANT SELECT ON nosuchdb.t TO 's1'@'%'
GRANT FOO ON *.* TO 's1'@'%'
GRANT FOO ON v.* TO 's1'@'%'
GRANT RELOAD ON v.* TO 's1'@'%'
GRANT RELOAD ON v.t TO 's1'@'%'
GRANT SELECT, SUPER ON v.t TO 's1'@'%'
GRANT ROLE_ADMIN ON v.* TO 's1'@'%'
GRANT DELETE (i) ON v.t TO 's1'@'%'
GRANT SELECT (nosuch) ON v.t TO 's1'@'%'
GRANT SELECT (i) ON v.* TO 's1'@'%'
GRANT SELECT ON * TO 's1'@'%'
GRANT USAGE ON *.* TO 's1'@'%'
GRANT GRANT OPTION ON v.t TO 's1'@'%'
SHOW GRANTS FOR 's1'@'%'
REVOKE GRANT OPTION ON v.t FROM 's1'@'%'
GRANT INSERT (i), SELECT (i, id), UPDATE (i) ON v.t TO 's1'@'%'
GRANT DELETE ON v.t TO 's1'@'%'
SHOW GRANTS FOR 's1'@'%'
GRANT SELECT ON v.u TO 's1'@'%', 's2'@'%'
SHOW GRANTS FOR 's2'@'%'
REVOKE ALL PRIVILEGES ON v.t FROM 's1'@'%'
SHOW GRANTS FOR 's1'@'%'
GRANT ALL ON v.t TO 's1'@'%'
REVOKE INSERT ON v.t FROM 's1'@'%'
SHOW GRANTS FOR 's1'@'%'
GRANT ALL ON *.* TO 's2'@'%' WITH GRANT OPTION
SHOW GRANTS FOR 's2'@'%'
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'nobody'@'%'
REVOKE ALL PRIVILEGES, GRANT OPTION FROM 's2'@'%'
ALTER USER 's1'@'%' IDENTIFIED BY 'x'
SET PASSWORD FOR 'nobody'@'%' = 'x'
RENAME USER 's1'@'%' TO 's2'@'%'
DROP USER 's1'@'%', 'nobody'@'%'
CREATE USER 's1'@'%', 's2'@'%'
GRANT 'ra' TO 'nobody'@'%'
GRANT 'ra' TO 'ra'
SET ROLE 'ra'
SET DEFAULT ROLE 'nosuch' TO 's7'@'%'
SET DEFAULT ROLE ALL TO 's7'@'%'
RENAME USER 's7'@'%' TO 's7b'@'%'
SHOW GRANTS FOR 's7b'@'%'
DROP USER 's1'@'%', 's2'@'%', 's3'@'%', 's4'@'%', 's5'@'%', 's6'@'%', 's7b'@'%', 's8'@'%'
DROP ROLE 'ra', 'rb'
# a login that names a database needs access to it (1044), DESCRIBE / SHOW COLUMNS / information_schema.columns list the columns the account holds a privilege on (R79)
--DROP USER IF EXISTS 'colonly'@'%'
--DROP USER IF EXISTS 'twocol'@'%'
--DROP USER IF EXISTS 'tblsel'@'%'
--DROP USER IF EXISTS 'nopriv'@'%'
--DROP USER IF EXISTS 'globalsel'@'%'
--DROP USER IF EXISTS 'otherdb'@'%'
--DROP USER IF EXISTS 'tblonly'@'%'
--DROP DATABASE IF EXISTS other79
--CREATE DATABASE other79
--CREATE TABLE other79.o (x INT)
--CREATE TABLE pc (a INT PRIMARY KEY, b INT, c INT, KEY ib (b))
--INSERT INTO pc VALUES (1,2,3)
--CREATE USER 'colonly'@'%' IDENTIFIED BY 'pw'
--GRANT SELECT (a) ON v.pc TO 'colonly'@'%'
--CREATE USER 'twocol'@'%' IDENTIFIED BY 'pw'
--GRANT SELECT (a), INSERT (c) ON v.pc TO 'twocol'@'%'
--CREATE USER 'tblsel'@'%' IDENTIFIED BY 'pw'
--GRANT SELECT ON v.pc TO 'tblsel'@'%'
--CREATE USER 'nopriv'@'%' IDENTIFIED BY 'pw'
--CREATE USER 'globalsel'@'%' IDENTIFIED BY 'pw'
--GRANT SELECT ON *.* TO 'globalsel'@'%'
--CREATE USER 'otherdb'@'%' IDENTIFIED BY 'pw'
--GRANT SELECT ON other79.* TO 'otherdb'@'%'
--CREATE USER 'tblonly'@'%' IDENTIFIED BY 'pw'
--GRANT SELECT ON v.t TO 'tblonly'@'%'
@colonly DESCRIBE pc
@colonly SHOW COLUMNS FROM pc
@colonly SHOW COLUMNS FROM pc LIKE 'b'
@colonly SHOW COLUMNS FROM pc LIKE 'a'
@colonly SELECT column_name FROM information_schema.columns WHERE table_name = 'pc' ORDER BY ordinal_position
@colonly SELECT a FROM pc
@colonly SELECT b FROM pc
@colonly SELECT DATABASE()
@twocol DESCRIBE pc
@twocol SHOW COLUMNS FROM pc
@twocol SELECT column_name FROM information_schema.columns WHERE table_name = 'pc' ORDER BY ordinal_position
@tblsel DESCRIBE pc
@tblsel SHOW COLUMNS FROM pc
@tblonly DESCRIBE pc
@tblonly SHOW COLUMNS FROM pc
@tblonly SELECT DATABASE()
@tblonly SHOW TABLES
@nopriv SELECT 1
@nopriv SELECT DATABASE()
@nopriv SHOW DATABASES
@globalsel SELECT DATABASE()
@globalsel DESCRIBE pc
@otherdb SELECT 1
@otherdb SELECT DATABASE()
--DROP USER 'colonly'@'%', 'twocol'@'%', 'tblsel'@'%', 'nopriv'@'%', 'globalsel'@'%', 'otherdb'@'%', 'tblonly'@'%'
--DROP DATABASE other79
