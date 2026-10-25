# ORDER BY the alias or the position of a scalar subquery of the select list (R52): it used to be ignored (the rows came out in table order) or refused.
--CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(20), dept INT, mgr INT, sal INT)
--CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(20), budget INT)
--INSERT INTO emp VALUES (1,'Ann',1,NULL,900),(2,'Bob',1,1,500),(3,'Cid',2,1,700),(4,'Dee',2,3,400),(5,'Eve',3,3,650),(6,'Fay',NULL,2,300)
--INSERT INTO dept VALUES (1,'Eng',1000),(2,'Ops',800),(3,'HR',300),(4,'Fin',500)
SELECT e.name, (SELECT COUNT(*) FROM emp x WHERE x.mgr = e.id) AS reports FROM emp e ORDER BY reports DESC, e.name
SELECT e.name, (SELECT COUNT(*) FROM emp x WHERE x.mgr = e.id) AS reports FROM emp e ORDER BY reports, e.name
SELECT e.name, (SELECT COUNT(*) FROM emp x WHERE x.mgr = e.id) AS reports FROM emp e ORDER BY 2 DESC, 1
SELECT e.name, (SELECT COUNT(*) FROM emp x WHERE x.mgr = e.id) reports FROM emp e ORDER BY reports DESC, e.id LIMIT 3
SELECT e.name, (SELECT COUNT(*) FROM emp x WHERE x.mgr = e.id) FROM emp e ORDER BY 2 DESC, 1
SELECT name, (SELECT COUNT(*) FROM emp x WHERE x.dept = emp.dept) AS cnt FROM emp ORDER BY cnt, name
SELECT name, (SELECT MAX(x.sal) FROM emp x WHERE x.dept = emp.dept) AS top FROM emp ORDER BY top DESC, name
SELECT name, (SELECT d.name FROM dept d WHERE d.id = e.dept) AS dname FROM emp e ORDER BY dname DESC, name
SELECT name, (SELECT d.budget FROM dept d WHERE d.id = e.dept) AS b, sal FROM emp e ORDER BY b DESC, sal DESC
SELECT name, (SELECT COUNT(*) FROM emp x WHERE x.mgr = e.id) AS r, (SELECT SUM(x.sal) FROM emp x WHERE x.mgr = e.id) AS s FROM emp e ORDER BY s DESC, name
SELECT e.name, d.name AS dn, (SELECT COUNT(*) FROM emp x WHERE x.dept = d.id) AS n FROM emp e JOIN dept d ON e.dept = d.id ORDER BY n DESC, e.name
SELECT name, (SELECT MAX(budget) FROM dept) AS m FROM emp ORDER BY m, name DESC
SELECT d.name, (SELECT SUM(e.sal) FROM emp e WHERE e.dept = d.id) AS payroll FROM dept d ORDER BY payroll
SELECT d.name, (SELECT SUM(e.sal) FROM emp e WHERE e.dept = d.id) AS payroll FROM dept d ORDER BY payroll DESC
SELECT d.name, (SELECT SUM(e.sal) FROM emp e WHERE e.dept = d.id) AS payroll FROM dept d WHERE d.id > 1 ORDER BY payroll DESC LIMIT 2
SELECT d.name, (SELECT COUNT(*) FROM emp e WHERE e.dept = d.id) AS n FROM dept d ORDER BY n DESC, d.name LIMIT 2 OFFSET 1
SELECT DISTINCT (SELECT COUNT(*) FROM emp x WHERE x.dept = e.dept) AS n FROM emp e ORDER BY n
SELECT DISTINCT (SELECT d.name FROM dept d WHERE d.id = e.dept) AS dn FROM emp e ORDER BY dn
SELECT DISTINCT e.dept, (SELECT COUNT(*) FROM emp x WHERE x.dept = e.dept) AS n FROM emp e ORDER BY e.dept
