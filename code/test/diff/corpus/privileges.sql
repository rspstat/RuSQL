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
