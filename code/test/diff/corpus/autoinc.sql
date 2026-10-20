# AUTO_INCREMENT numbers a statement takes and does not use (R44): InnoDB reserves them -- N for an INSERT ... VALUES of N rows, 1 then 2, 4, 8 ... for an
# INSERT ... SELECT -- a row that is skipped or fails gives none back, what is left is lost, and a statement whose first row fails the checks of its columns reserved
# nothing. Each scenario is a table with two rows (ids 1 and 2); the last probe row says which number is next. A line is a statement, '--' a setup statement.
--CREATE TABLE ai_p (id INT PRIMARY KEY)
--INSERT INTO ai_p VALUES (1), (2)
# S1 single plain dup
--CREATE TABLE ai_t0 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t0 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t0 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t0 (v, u) VALUES ('b', 2)
INSERT INTO ai_t0 (v,u) VALUES ('x',1)
INSERT INTO ai_t0 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t0 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t0
# S2 single ignore dup
--CREATE TABLE ai_t1 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t1 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t1 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t1 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t1 (v,u) VALUES ('x',1)
INSERT INTO ai_t1 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t1 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t1
# S3 single odku update
--CREATE TABLE ai_t2 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t2 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t2 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t2 (v, u) VALUES ('b', 2)
INSERT INTO ai_t2 (v,u) VALUES ('x',1) ON DUPLICATE KEY UPDATE v='k'
INSERT INTO ai_t2 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t2 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t2
# S4 single odku insert
--CREATE TABLE ai_t3 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t3 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t3 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t3 (v, u) VALUES ('b', 2)
INSERT INTO ai_t3 (v,u) VALUES ('x',50) ON DUPLICATE KEY UPDATE v='k'
INSERT INTO ai_t3 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t3 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t3
# S5 multi plain 3 ok
--CREATE TABLE ai_t4 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t4 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t4 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t4 (v, u) VALUES ('b', 2)
INSERT INTO ai_t4 (v,u) VALUES ('x',50),('y',51),('w',52)
INSERT INTO ai_t4 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t4 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t4
# S6 multi plain ok,dup,ok
--CREATE TABLE ai_t5 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t5 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t5 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t5 (v, u) VALUES ('b', 2)
INSERT INTO ai_t5 (v,u) VALUES ('x',50),('y',1),('w',52)
INSERT INTO ai_t5 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t5 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t5
# S7 multi plain dup,ok,ok
--CREATE TABLE ai_t6 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t6 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t6 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t6 (v, u) VALUES ('b', 2)
INSERT INTO ai_t6 (v,u) VALUES ('x',1),('y',51),('w',52)
INSERT INTO ai_t6 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t6 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t6
# S8 multi ignore ok,dup,ok
--CREATE TABLE ai_t7 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t7 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t7 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t7 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t7 (v,u) VALUES ('x',50),('y',1),('w',52)
INSERT INTO ai_t7 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t7 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t7
# S9 multi ignore dup,ok,ok
--CREATE TABLE ai_t8 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t8 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t8 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t8 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t8 (v,u) VALUES ('x',1),('y',51),('w',52)
INSERT INTO ai_t8 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t8 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t8
# S9b multi ignore ok,ok,dup
--CREATE TABLE ai_t9 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t9 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t9 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t9 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t9 (v,u) VALUES ('x',50),('y',51),('w',1)
INSERT INTO ai_t9 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t9 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t9
# S9c multi ignore dup,dup,ok
--CREATE TABLE ai_t10 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t10 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t10 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t10 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t10 (v,u) VALUES ('x',1),('y',2),('w',52)
INSERT INTO ai_t10 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t10 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t10
# S10 multi odku upd,ok,ok
--CREATE TABLE ai_t11 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t11 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t11 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t11 (v, u) VALUES ('b', 2)
INSERT INTO ai_t11 (v,u) VALUES ('x',1),('y',51),('w',52) ON DUPLICATE KEY UPDATE v='k'
INSERT INTO ai_t11 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t11 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t11
# S11 multi odku ok,upd,ok
--CREATE TABLE ai_t12 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t12 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t12 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t12 (v, u) VALUES ('b', 2)
INSERT INTO ai_t12 (v,u) VALUES ('x',50),('y',1),('w',52) ON DUPLICATE KEY UPDATE v='k'
INSERT INTO ai_t12 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t12 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t12
# S12 replace single conflict
--CREATE TABLE ai_t13 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t13 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t13 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t13 (v, u) VALUES ('b', 2)
REPLACE INTO ai_t13 (v,u) VALUES ('x',1)
INSERT INTO ai_t13 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t13 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t13
# S12b replace multi conflict
--CREATE TABLE ai_t14 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t14 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t14 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t14 (v, u) VALUES ('b', 2)
REPLACE INTO ai_t14 (v,u) VALUES ('x',1),('y',50),('w',2)
INSERT INTO ai_t14 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t14 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t14
# S13 not null single
--CREATE TABLE ai_t15 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t15 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t15 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t15 (v, u) VALUES ('b', 2)
INSERT INTO ai_t15 (v,u) VALUES (NULL,50)
INSERT INTO ai_t15 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t15 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t15
# S13b not null multi ok,bad,ok
--CREATE TABLE ai_t16 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t16 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t16 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t16 (v, u) VALUES ('b', 2)
INSERT INTO ai_t16 (v,u) VALUES ('x',50),(NULL,51),('w',52)
INSERT INTO ai_t16 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t16 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t16
# S13c too long single
--CREATE TABLE ai_t17 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t17 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t17 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t17 (v, u) VALUES ('b', 2)
INSERT INTO ai_t17 (v,u) VALUES ('toolong',50)
INSERT INTO ai_t17 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t17 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t17
# S13d too long multi
--CREATE TABLE ai_t18 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t18 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t18 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t18 (v, u) VALUES ('b', 2)
INSERT INTO ai_t18 (v,u) VALUES ('x',50),('toolong',51),('w',52)
INSERT INTO ai_t18 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t18 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t18
# S13e check single
--CREATE TABLE ai_t19 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t19 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t19 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t19 (v, u) VALUES ('b', 2)
INSERT INTO ai_t19 (v,u,q) VALUES ('x',50,500)
INSERT INTO ai_t19 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t19 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t19
# S13f check multi ok,bad,ok
--CREATE TABLE ai_t20 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t20 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t20 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t20 (v, u) VALUES ('b', 2)
INSERT INTO ai_t20 (v,u,q) VALUES ('x',50,1),('y',51,500),('w',52,2)
INSERT INTO ai_t20 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t20 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t20
# S13g fk single
--CREATE TABLE ai_t21 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t21 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t21 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t21 (v, u) VALUES ('b', 2)
INSERT INTO ai_t21 (v,u,q) VALUES ('x',50,77)
INSERT INTO ai_t21 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t21 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t21
# S13h fk multi ok,bad,ok
--CREATE TABLE ai_t22 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t22 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t22 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t22 (v, u) VALUES ('b', 2)
INSERT INTO ai_t22 (v,u,q) VALUES ('x',50,1),('y',51,77),('w',52,2)
INSERT INTO ai_t22 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t22 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t22
# S14 select 1
--CREATE TABLE ai_t23 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t23 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t23 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t23 (v, u) VALUES ('b', 2)
INSERT INTO ai_t23 (v,u) SELECT 'x',50 FROM ai_p WHERE id=1
INSERT INTO ai_t23 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t23 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t23
# S14b select 2
--CREATE TABLE ai_t24 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t24 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t24 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t24 (v, u) VALUES ('b', 2)
INSERT INTO ai_t24 (v,u) SELECT 'x',50+id FROM ai_p
INSERT INTO ai_t24 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t24 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t24
# S14c select 3 (p x p limited)
--CREATE TABLE ai_t25 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t25 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t25 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t25 (v, u) VALUES ('b', 2)
INSERT INTO ai_t25 (v,u) SELECT 'x',50+a.id*10+b.id FROM ai_p a, ai_p b LIMIT 3
INSERT INTO ai_t25 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t25 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t25
# S14d select 4
--CREATE TABLE ai_t26 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t26 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t26 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t26 (v, u) VALUES ('b', 2)
INSERT INTO ai_t26 (v,u) SELECT 'x',50+a.id*10+b.id FROM ai_p a, ai_p b
INSERT INTO ai_t26 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t26 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t26
# S14e select 5 (union)
--CREATE TABLE ai_t27 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t27 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t27 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t27 (v, u) VALUES ('b', 2)
INSERT INTO ai_t27 (v,u) SELECT 'x',50+a.id*10+b.id FROM ai_p a, ai_p b UNION ALL SELECT 'y', 99
INSERT INTO ai_t27 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t27 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t27
# S15 select ignore dup mid
--CREATE TABLE ai_t28 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t28 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t28 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t28 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t28 (v,u) SELECT 'x', 1+id*0+(id-1)*49 FROM ai_p
INSERT INTO ai_t28 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t28 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t28
# S16 explicit then auto
--CREATE TABLE ai_t29 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t29 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t29 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t29 (v, u) VALUES ('b', 2)
INSERT INTO ai_t29 (id,v,u) VALUES (10,'x',50)
INSERT INTO ai_t29 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t29 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t29
# S17 mixed explicit/null multi
--CREATE TABLE ai_t30 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t30 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t30 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t30 (v, u) VALUES ('b', 2)
INSERT INTO ai_t30 (id,v,u) VALUES (NULL,'x',50),(20,'y',51),(NULL,'w',52)
INSERT INTO ai_t30 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t30 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t30
# S17b zero/explicit multi
--CREATE TABLE ai_t31 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t31 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t31 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t31 (v, u) VALUES ('b', 2)
INSERT INTO ai_t31 (id,v,u) VALUES (0,'x',50),(5,'y',51),(0,'w',52)
INSERT INTO ai_t31 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t31 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t31
# S18 txn rollback
--CREATE TABLE ai_t32 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t32 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t32 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t32 (v, u) VALUES ('b', 2)
START TRANSACTION
INSERT INTO ai_t32 (v,u) VALUES ('x',50),('y',51)
ROLLBACK
INSERT INTO ai_t32 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t32 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t32
# S19 delete then insert
--CREATE TABLE ai_t33 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t33 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t33 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t33 (v, u) VALUES ('b', 2)
DELETE FROM ai_t33 WHERE id=2
INSERT INTO ai_t33 (v,u) VALUES ('x',50)
INSERT INTO ai_t33 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t33 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t33
# S20 odku no change
--CREATE TABLE ai_t34 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t34 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t34 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t34 (v, u) VALUES ('b', 2)
INSERT INTO ai_t34 (v,u) VALUES ('x',1) ON DUPLICATE KEY UPDATE v=v
INSERT INTO ai_t34 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t34 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t34
# S21 not null multi bad,ok,ok
--CREATE TABLE ai_t35 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t35 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t35 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t35 (v, u) VALUES ('b', 2)
INSERT INTO ai_t35 (v,u) VALUES (NULL,50),('y',51),('w',52)
INSERT INTO ai_t35 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t35 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t35
# S22 fk multi bad,ok,ok
--CREATE TABLE ai_t36 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t36 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t36 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t36 (v, u) VALUES ('b', 2)
INSERT INTO ai_t36 (v,u,q) VALUES ('x',50,77),('y',51,1),('w',52,2)
INSERT INTO ai_t36 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t36 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t36
# S23 ignore all dup
--CREATE TABLE ai_t37 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t37 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t37 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t37 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t37 (v,u) VALUES ('x',1),('y',2),('w',1)
INSERT INTO ai_t37 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t37 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t37
# S24 too long multi first
--CREATE TABLE ai_t38 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t38 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t38 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t38 (v, u) VALUES ('b', 2)
INSERT INTO ai_t38 (v,u) VALUES ('toolong',50),('y',51)
INSERT INTO ai_t38 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t38 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t38
# S25 check multi first bad
--CREATE TABLE ai_t39 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t39 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t39 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t39 (v, u) VALUES ('b', 2)
INSERT INTO ai_t39 (v,u,q) VALUES ('x',50,500),('y',51,1)
INSERT INTO ai_t39 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t39 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t39
# S26 plain multi dup within stmt
--CREATE TABLE ai_t40 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t40 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t40 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t40 (v, u) VALUES ('b', 2)
INSERT INTO ai_t40 (v,u) VALUES ('x',50),('y',50),('w',52)
INSERT INTO ai_t40 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t40 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t40
# S27 odku multi dup within stmt
--CREATE TABLE ai_t41 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t41 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t41 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t41 (v, u) VALUES ('b', 2)
INSERT INTO ai_t41 (v,u) VALUES ('x',50),('y',50),('w',52) ON DUPLICATE KEY UPDATE v='k'
INSERT INTO ai_t41 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t41 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t41
# S28 ignore dup within stmt
--CREATE TABLE ai_t42 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t42 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t42 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t42 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t42 (v,u) VALUES ('x',50),('y',50),('w',52)
INSERT INTO ai_t42 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t42 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t42
# S29 two ignore stmts
--CREATE TABLE ai_t43 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t43 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t43 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t43 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t43 (v,u) VALUES ('x',1)
INSERT IGNORE INTO ai_t43 (v,u) VALUES ('y',2),('w',60)
INSERT INTO ai_t43 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t43 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t43
# S30 select ignore all dup 3
--CREATE TABLE ai_t44 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, u INT UNIQUE, q INT, CHECK (q IS NULL OR q < 100), CONSTRAINT fk_ai_t44 FOREIGN KEY (q) REFERENCES ai_p(id))
--INSERT INTO ai_t44 (v, u) VALUES ('a', 1)
--INSERT INTO ai_t44 (v, u) VALUES ('b', 2)
INSERT IGNORE INTO ai_t44 (v,u) SELECT 'x', 1 FROM ai_p a, ai_p b LIMIT 3
INSERT INTO ai_t44 (v, u) VALUES ('z', 999)
SELECT id FROM ai_t44 WHERE u = 999
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_t44

# a row that repeats a key of an earlier row of the same statement: IGNORE skips it, ON DUPLICATE KEY UPDATE updates the earlier row
--CREATE TABLE ct (k VARCHAR(5) PRIMARY KEY, n INT, note VARCHAR(10))
INSERT INTO ct (k, n) VALUES ('a', 1), ('a', 1), ('b', 5), ('a', 1) ON DUPLICATE KEY UPDATE n = n + VALUES(n), note = CONCAT('s', n)
SELECT k, n, note FROM ct ORDER BY k
INSERT INTO ct (k, n) VALUES ('a', 10), ('c', 1), ('a', 10) ON DUPLICATE KEY UPDATE n = n + VALUES(n)
SELECT k, n, note FROM ct ORDER BY k
INSERT IGNORE INTO ct (k, n) VALUES ('d', 1), ('d', 2), ('e', 3)
SELECT k, n FROM ct ORDER BY k
--CREATE TABLE cp (a INT, b INT, v INT, PRIMARY KEY (a, b))
INSERT IGNORE INTO cp VALUES (1, 1, 1), (1, 1, 2), (2, 2, 3)
SELECT a, b, v FROM cp ORDER BY a, b
INSERT INTO cp VALUES (3, 3, 3), (3, 3, 4)
SELECT COUNT(*) FROM cp
--CREATE TABLE cu (id INT PRIMARY KEY AUTO_INCREMENT, a INT, b INT, v INT, UNIQUE KEY ab (a, b))
INSERT IGNORE INTO cu (a, b, v) VALUES (1, 1, 1), (1, 1, 2), (2, 2, 3)
SELECT id, a, b, v FROM cu ORDER BY id

# LAST_INSERT_ID() is the first number a row took; a skipped row takes none. An INSERT through a view numbers its rows like one into the table
--CREATE TABLE ai_li (id INT PRIMARY KEY AUTO_INCREMENT, u INT UNIQUE)
INSERT INTO ai_li (u) VALUES (1)
SELECT LAST_INSERT_ID()
INSERT IGNORE INTO ai_li (u) VALUES (1), (5)
SELECT LAST_INSERT_ID()
INSERT IGNORE INTO ai_li (u) VALUES (1), (5)
SELECT LAST_INSERT_ID()
INSERT IGNORE INTO ai_li (u) VALUES (9), (10)
SELECT LAST_INSERT_ID()
SELECT id, u FROM ai_li ORDER BY id
--CREATE TABLE ai_vt (id INT PRIMARY KEY AUTO_INCREMENT, n INT)
--CREATE VIEW ai_vv AS SELECT id, n FROM ai_vt
INSERT INTO ai_vv (n) SELECT a.id*10+b.id FROM ai_p a, ai_p b
INSERT INTO ai_vt (n) VALUES (0)
SELECT id FROM ai_vt WHERE n = 0
SELECT LAST_INSERT_ID()

# a check on a column is made before the row is written: a first row that fails it burns no number, a later row that fails it keeps the reservation burned
--CREATE TABLE ai_cc1 (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(3) NOT NULL, q INT CHECK (q < 100))
--INSERT INTO ai_cc1 (v, q) VALUES ('a', 1), ('b', 2)
INSERT INTO ai_cc1 (v, q) VALUES ('x', 500)
INSERT INTO ai_cc1 (v, q) VALUES ('y', 500), ('w', 1)
INSERT INTO ai_cc1 (v, q) VALUES ('w', 1), ('y', 500), ('x', 2)
INSERT INTO ai_cc1 (v, q) VALUES ('z', 1)
SELECT id FROM ai_cc1 WHERE v = 'z'
SELECT GROUP_CONCAT(id ORDER BY id) FROM ai_cc1

# the assignments of an ON DUPLICATE KEY UPDATE that hits a row of the same statement give the column what it holds (DECIMAL(5,2): 1 / 3 is 0.33)
--CREATE TABLE ct2 (k VARCHAR(5) PRIMARY KEY, d DECIMAL(5,2), s VARCHAR(4))
INSERT INTO ct2 (k, d, s) VALUES ('a', 1, 'x'), ('a', 1, 'x') ON DUPLICATE KEY UPDATE d = d / 3
SELECT k, d, s FROM ct2
INSERT INTO ct2 (k, d, s) VALUES ('b', 1, 'x'), ('b', 1, 'x') ON DUPLICATE KEY UPDATE s = CONCAT(s, 'yyyyy')
SELECT k, d, s FROM ct2 ORDER BY k
