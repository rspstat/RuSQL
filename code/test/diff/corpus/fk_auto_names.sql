# The names MySQL gives a foreign key that has none, and how it reports the rules (R58).
# CREATE TABLE numbers the keys it was not given a name for <table>_ibfk_1, _2 ... in the order they are written (not the order of their columns); ALTER TABLE ... ADD FOREIGN KEY takes
# one more than the highest <table>_ibfk_<n> the table has; DROP FOREIGN KEY leaves the others as they are; RENAME TABLE changes the prefix. A rule left out (or NO ACTION) is reported
# as NO ACTION in information_schema.referential_constraints, RESTRICT only when it was written; SHOW CREATE TABLE prints a RESTRICT that was written and no NO ACTION.
--CREATE TABLE fa_p1 (id INT PRIMARY KEY)
--CREATE TABLE fa_p2 (id INT PRIMARY KEY)
--CREATE TABLE fa_p3 (id INT PRIMARY KEY)
--CREATE TABLE fa_t (a INT, b INT, FOREIGN KEY (b) REFERENCES fa_p1(id), FOREIGN KEY (a) REFERENCES fa_p2(id))
SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_t' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_t DROP FOREIGN KEY fa_t_ibfk_1
SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_t' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_t ADD FOREIGN KEY (b) REFERENCES fa_p3(id)
SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_t' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_t ADD FOREIGN KEY (a) REFERENCES fa_p3(id)
SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_t' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_t DROP FOREIGN KEY fa_t_ibfk_4
ALTER TABLE fa_t ADD FOREIGN KEY (a) REFERENCES fa_p1(id)
SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_t' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
RENAME TABLE fa_t TO fa_u
SELECT constraint_name, column_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_u' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_u RENAME TO fa_w
SELECT constraint_name FROM information_schema.referential_constraints WHERE constraint_schema = 'v' AND table_name = 'fa_w' ORDER BY constraint_name
ALTER TABLE fa_w DROP FOREIGN KEY fa_w_ibfk_2
SELECT constraint_name FROM information_schema.referential_constraints WHERE constraint_schema = 'v' AND table_name = 'fa_w' ORDER BY constraint_name
--CREATE TABLE fa_n (a INT, b INT, CONSTRAINT c1 FOREIGN KEY (a) REFERENCES fa_p1(id), FOREIGN KEY (b) REFERENCES fa_p2(id))
SELECT constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_n' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
--CREATE TABLE fa_m (a INT, b INT, FOREIGN KEY (a) REFERENCES fa_p1(id), CONSTRAINT c9 FOREIGN KEY (b) REFERENCES fa_p2(id))
SELECT constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_m' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
--CREATE TABLE fa_x (a INT, b INT, CONSTRAINT fa_x_ibfk_5 FOREIGN KEY (a) REFERENCES fa_p1(id), FOREIGN KEY (b) REFERENCES fa_p2(id))
SELECT constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_x' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_x ADD FOREIGN KEY (a) REFERENCES fa_p3(id)
SELECT constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_x' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
--CREATE TABLE fa_y (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id), FOREIGN KEY (a) REFERENCES fa_p2(id))
ALTER TABLE fa_y DROP FOREIGN KEY fa_y_ibfk_1, ADD FOREIGN KEY (a) REFERENCES fa_p3(id)
SELECT constraint_name, referenced_table_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_y' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
--CREATE TABLE fa_r1 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id))
--CREATE TABLE fa_r2 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id) ON DELETE RESTRICT ON UPDATE RESTRICT)
--CREATE TABLE fa_r3 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id) ON DELETE NO ACTION ON UPDATE NO ACTION)
--CREATE TABLE fa_r4 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id) ON DELETE CASCADE)
--CREATE TABLE fa_r5 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id) ON DELETE SET NULL ON UPDATE CASCADE)
--CREATE TABLE fa_r6 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id) ON UPDATE RESTRICT)
--CREATE TABLE fa_r7 (a INT, FOREIGN KEY (a) REFERENCES fa_p1(id) ON DELETE NO ACTION)
SELECT table_name, update_rule, delete_rule FROM information_schema.referential_constraints WHERE constraint_schema = 'v' AND table_name LIKE 'fa_r%' ORDER BY table_name
ALTER TABLE fa_r1 ADD CONSTRAINT fa_r1_x FOREIGN KEY (a) REFERENCES fa_p2(id) ON DELETE RESTRICT
ALTER TABLE fa_r1 ADD CONSTRAINT fa_r1_y FOREIGN KEY (a) REFERENCES fa_p3(id) ON UPDATE NO ACTION
SELECT constraint_name, update_rule, delete_rule FROM information_schema.referential_constraints WHERE constraint_schema = 'v' AND table_name = 'fa_r1' ORDER BY constraint_name
INSERT INTO fa_p1 VALUES (1)
INSERT INTO fa_r2 VALUES (1)
DELETE FROM fa_p1 WHERE id = 1
UPDATE fa_p1 SET id = 5 WHERE id = 1
INSERT INTO fa_r3 VALUES (1)
DELETE FROM fa_p1 WHERE id = 1
DELETE FROM fa_r2
DELETE FROM fa_r3
DELETE FROM fa_p1 WHERE id = 1
--CREATE TABLE fa_pp (id INT PRIMARY KEY)
--CREATE TABLE fa_cc (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES fa_pp(id))
--INSERT INTO fa_pp VALUES (1)
--INSERT INTO fa_cc VALUES (1, 1)
RENAME TABLE fa_pp TO fa_pq
SELECT constraint_name, referenced_table_name FROM information_schema.referential_constraints WHERE constraint_schema = 'v' AND table_name = 'fa_cc'
INSERT INTO fa_cc VALUES (2, 9)
DELETE FROM fa_pq WHERE id = 1
INSERT INTO fa_pq VALUES (2)
INSERT INTO fa_cc VALUES (3, 2)
DELETE FROM fa_pq WHERE id = 2
--CREATE TABLE fa_h (a INT, b INT, FOREIGN KEY (a) REFERENCES fa_p1(id), FOREIGN KEY (b) REFERENCES fa_p2(id))
ALTER TABLE fa_h DROP FOREIGN KEY fa_h_ibfk_2
ALTER TABLE fa_h ADD FOREIGN KEY (b) REFERENCES fa_p3(id)
SELECT constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_h' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
ALTER TABLE fa_h DROP FOREIGN KEY fa_h_ibfk_1
ALTER TABLE fa_h ADD FOREIGN KEY (a) REFERENCES fa_p3(id)
SELECT constraint_name, column_name FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name = 'fa_h' AND referenced_table_name IS NOT NULL ORDER BY constraint_name
--CREATE TABLE fa_z (a INT, CONSTRAINT fa_z_ibfk_x FOREIGN KEY (a) REFERENCES fa_p1(id))
ALTER TABLE fa_z ADD FOREIGN KEY (a) REFERENCES fa_p2(id)
SELECT constraint_name FROM information_schema.referential_constraints WHERE constraint_schema = 'v' AND table_name = 'fa_z' ORDER BY constraint_name
