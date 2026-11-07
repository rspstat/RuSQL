# Statements inside a transaction and the isolation level (R71): BEGIN / SAVEPOINT / ROLLBACK TO / COMMIT, what a ROLLBACK takes back (rows, auto numbers, DDL that has committed), SET autocommit,
# the isolation level and what @@transaction_isolation says after each way of setting it, READ ONLY transactions. A line is a statement, '--' a setup statement run on both.
--CREATE TABLE tx (id INT AUTO_INCREMENT PRIMARY KEY, v INT, name VARCHAR(10), UNIQUE KEY (name))
--INSERT INTO tx (v, name) VALUES (10, 'a'), (20, 'b'), (30, 'c')
BEGIN
UPDATE tx SET v = v + 1 WHERE name = 'a'
SELECT * FROM tx ORDER BY id
SAVEPOINT s1
DELETE FROM tx WHERE name = 'b'
SELECT * FROM tx ORDER BY id
SAVEPOINT s2
INSERT INTO tx (v, name) VALUES (40, 'd')
ROLLBACK TO SAVEPOINT s2
SELECT * FROM tx ORDER BY id
ROLLBACK TO SAVEPOINT s1
SELECT * FROM tx ORDER BY id
ROLLBACK TO SAVEPOINT s2
RELEASE SAVEPOINT s1
ROLLBACK TO SAVEPOINT s1
ROLLBACK
SELECT * FROM tx ORDER BY id
# a duplicate key inside a transaction fails that statement only
BEGIN
INSERT INTO tx (v, name) VALUES (50, 'e')
INSERT INTO tx (v, name) VALUES (60, 'e')
SELECT * FROM tx ORDER BY id
COMMIT
SELECT * FROM tx ORDER BY id
# the auto number is not given back by a ROLLBACK
BEGIN
INSERT INTO tx (v, name) VALUES (70, 'f')
ROLLBACK
INSERT INTO tx (v, name) VALUES (80, 'g')
SELECT id, name FROM tx WHERE name IN ('f', 'g') ORDER BY id
# a statement that fails in the middle changes nothing of its own, the transaction goes on
BEGIN
UPDATE tx SET name = 'a' WHERE name IN ('b', 'c', 'e')
SELECT name FROM tx ORDER BY id
COMMIT
# DDL commits the transaction before it
BEGIN
INSERT INTO tx (v, name) VALUES (90, 'h')
CREATE TABLE tx_other (id INT)
ROLLBACK
SELECT COUNT(*) FROM tx WHERE name = 'h'
BEGIN
INSERT INTO tx (v, name) VALUES (91, 'i')
TRUNCATE TABLE tx_other
ROLLBACK
SELECT COUNT(*) FROM tx WHERE name = 'i'
# START TRANSACTION forms
START TRANSACTION
DELETE FROM tx WHERE name = 'h'
ROLLBACK
SELECT COUNT(*) FROM tx WHERE name = 'h'
START TRANSACTION READ WRITE
DELETE FROM tx WHERE name = 'h'
ROLLBACK
START TRANSACTION WITH CONSISTENT SNAPSHOT
SELECT COUNT(*) FROM tx
COMMIT
START TRANSACTION READ ONLY
SELECT COUNT(*) FROM tx
INSERT INTO tx (v, name) VALUES (1, 'ro')
UPDATE tx SET v = 0
DELETE FROM tx
COMMIT
SELECT COUNT(*) FROM tx WHERE name = 'ro'
# autocommit
SELECT @@autocommit
SET autocommit = 0
SELECT @@autocommit
INSERT INTO tx (v, name) VALUES (2, 'ac')
ROLLBACK
SELECT COUNT(*) FROM tx WHERE name = 'ac'
INSERT INTO tx (v, name) VALUES (3, 'ac')
COMMIT
SELECT COUNT(*) FROM tx WHERE name = 'ac'
SET autocommit = 1
INSERT INTO tx (v, name) VALUES (4, 'ac2')
ROLLBACK
SELECT COUNT(*) FROM tx WHERE name = 'ac2'
# the isolation level and what @@transaction_isolation says. (Not compared: the level of a session that set nothing -- MySQL REPEATABLE-READ, RuSQL READ-COMMITTED, which is
# the level its engine runs at; and what SET transaction_isolation = DEFAULT gives back.)
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED
SELECT @@transaction_isolation
SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE
SELECT @@transaction_isolation, @@session.transaction_isolation
SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED
SELECT @@transaction_isolation
SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ
SELECT @@transaction_isolation
SET transaction_isolation = 'READ-COMMITTED'
SELECT @@transaction_isolation
SET SESSION transaction_isolation = 'SERIALIZABLE'
SELECT @@transaction_isolation
SET @@session.transaction_isolation = 'READ-UNCOMMITTED'
SELECT @@transaction_isolation
# (not compared: SET @@transaction_isolation = x without SESSION -- MySQL takes it for the next transaction only, RuSQL for the session)
SET transaction_isolation = 'read-committed'
SELECT @@transaction_isolation
SET transaction_isolation = 'SERIALIZABLE'
SELECT @@transaction_isolation
SET transaction_isolation = 'READ COMMITTED'
SET transaction_isolation = 'nonsense'
SET transaction_isolation = DEFAULT
SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ
SELECT @@transaction_isolation
# a SET TRANSACTION (no SESSION) is for the next transaction only
SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
SELECT @@transaction_isolation
BEGIN
SELECT @@transaction_isolation
COMMIT
SELECT @@transaction_isolation
# the isolation level cannot be changed in a transaction
BEGIN
SET TRANSACTION ISOLATION LEVEL READ COMMITTED
SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED
SET transaction_isolation = 'READ-COMMITTED'
COMMIT
# access mode
SET SESSION TRANSACTION READ ONLY
SELECT @@transaction_read_only
INSERT INTO tx (v, name) VALUES (5, 'ro2')
SET SESSION TRANSACTION READ WRITE
SELECT @@transaction_read_only
SET transaction_read_only = 1
INSERT INTO tx (v, name) VALUES (5, 'ro3')
SET transaction_read_only = 0
INSERT INTO tx (v, name) VALUES (5, 'rw')
SELECT COUNT(*) FROM tx WHERE name IN ('ro2', 'ro3', 'rw')
# COMMIT / ROLLBACK with nothing open, and the chain / release forms
COMMIT
ROLLBACK
COMMIT AND CHAIN
BEGIN
COMMIT AND NO CHAIN
BEGIN
ROLLBACK AND CHAIN
ROLLBACK
SAVEPOINT nothing_open
ROLLBACK TO SAVEPOINT nothing_open
RELEASE SAVEPOINT nothing_open
# FOR UPDATE / FOR SHARE / LOCK IN SHARE MODE inside and outside a transaction
SELECT * FROM tx WHERE name = 'a' FOR UPDATE
SELECT * FROM tx WHERE name = 'a' FOR SHARE
SELECT * FROM tx WHERE name = 'a' LOCK IN SHARE MODE
BEGIN
SELECT * FROM tx WHERE name = 'a' FOR UPDATE
SELECT * FROM tx WHERE name = 'zz' FOR UPDATE
SELECT * FROM tx WHERE name = 'a' FOR UPDATE NOWAIT
SELECT * FROM tx WHERE name = 'a' FOR UPDATE SKIP LOCKED
COMMIT
