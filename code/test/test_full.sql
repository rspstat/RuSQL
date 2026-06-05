-- Init
DROP USER IF EXISTS 'testuser'@'%';
DROP DATABASE IF EXISTS company;
CREATE DATABASE company;
USE company;

-- DDL: Tables
-- department : INT, VARCHAR, DECIMAL, BIGINT, SMALLINT, TINYINT, BOOLEAN, DATE, TIME, YEAR, TEXT, JSON, ENUM, SET
-- employee   : INT, BIGINT, VARCHAR, DATE, DATETIME, TIMESTAMP, DECIMAL, FLOAT, DOUBLE, BOOLEAN, TINYINT, SMALLINT, BLOB, JSON, TEXT, ENUM, SET
-- project    : INT, VARCHAR, TEXT, DECIMAL, BIGINT, SMALLINT, TINYINT, DATE, DATETIME, TIMESTAMP, FLOAT, DOUBLE, BOOLEAN, JSON, ENUM, SET
CREATE TABLE department (
    id            INT            AUTO INCREMENT,
    code          VARCHAR(10)    NOT NULL,
    name          VARCHAR(100)   NOT NULL,
    budget        DECIMAL(15,2)  DEFAULT 0.00 CHECK (budget >= 0),
    annual_target BIGINT         DEFAULT 0,
    headcount     SMALLINT       DEFAULT 0 CHECK (headcount >= 0),
    floor_num     TINYINT        DEFAULT 1,
    is_active     BOOLEAN        DEFAULT true,
    established   DATE,
    open_time     TIME,
    fiscal_year   YEAR,
    description   TEXT,
    metadata      JSON,
    dept_type     ENUM('engineering','sales','marketing','finance','hr','ops','legal'),
    perks         SET('gym','cafe','parking','library','childcare'),
    CONSTRAINT pk_dept     PRIMARY KEY (id),
    UNIQUE KEY uq_dept_code (code),
    UNIQUE KEY uq_dept_name (name)
);

CREATE TABLE employee (
    id               INT            AUTO INCREMENT,
    employee_code    BIGINT         NOT NULL,
    first_name       VARCHAR(50)    NOT NULL,
    last_name        VARCHAR(50)    NOT NULL,
    email            VARCHAR(100)   NOT NULL,
    birth_date       DATE,
    hire_date        DATE           NOT NULL,
    termination_date DATETIME,
    created_at       TIMESTAMP,
    salary           DECIMAL(12,2)  CHECK (salary > 0),
    hourly_rate      FLOAT          DEFAULT 0.0,
    performance      DOUBLE         DEFAULT 0.0 CHECK (performance BETWEEN 0.0 AND 10.0),
    department_id    INT,
    manager_id       INT,
    job_title        VARCHAR(100),
    emp_type         ENUM('full_time','part_time','contract','intern') DEFAULT 'full_time',
    skills           SET('python','java','rust','sql','ml','devops','design'),
    experience_years TINYINT        DEFAULT 0,
    is_manager       BOOLEAN        DEFAULT false,
    annual_leave     SMALLINT       DEFAULT 20,
    resume_data      BLOB,
    personal_data    JSON,
    bio              TEXT,
    CONSTRAINT pk_employee  PRIMARY KEY (id),
    UNIQUE KEY uq_emp_code  (employee_code),
    UNIQUE KEY uq_emp_email (email),
    CONSTRAINT fk_emp_dept  FOREIGN KEY (department_id) REFERENCES department(id) ON DELETE SET NULL ON UPDATE CASCADE
);

CREATE TABLE project (
    id            INT            AUTO INCREMENT,
    code          VARCHAR(20)    NOT NULL,
    name          VARCHAR(200)   NOT NULL,
    description   TEXT,
    budget        DECIMAL(15,2)  DEFAULT 0.00 CHECK (budget >= 0),
    alloc_hours   BIGINT         DEFAULT 0,
    team_size     SMALLINT       DEFAULT 1,
    priority      TINYINT        DEFAULT 3 CHECK (priority BETWEEN 1 AND 5),
    start_date    DATE,
    end_date      DATE,
    deadline      DATETIME,
    updated_at    TIMESTAMP,
    revenue       FLOAT          DEFAULT 0.0,
    completion    DOUBLE         DEFAULT 0.0 CHECK (completion BETWEEN 0.0 AND 100.0),
    department_id INT            NOT NULL,
    lead_id       INT,
    status        ENUM('planning','active','on_hold','completed','cancelled') DEFAULT 'planning',
    tech_stack    SET('frontend','backend','database','mobile','cloud','ai','security'),
    is_public     BOOLEAN        DEFAULT false,
    contract_data JSON,
    CONSTRAINT pk_project   PRIMARY KEY (id),
    UNIQUE KEY uq_proj_code (code),
    CONSTRAINT fk_proj_dept FOREIGN KEY (department_id) REFERENCES department(id) ON DELETE RESTRICT ON UPDATE CASCADE,
    CONSTRAINT fk_proj_lead FOREIGN KEY (lead_id)       REFERENCES employee(id)  ON DELETE CASCADE  ON UPDATE CASCADE
);

-- DDL: Indexes (B-tree)
CREATE INDEX idx_dept_type    ON department (dept_type);
CREATE INDEX idx_dept_active  ON department (is_active);
CREATE INDEX idx_emp_dept     ON employee (department_id);
CREATE INDEX idx_emp_type     ON employee (emp_type);
CREATE INDEX idx_emp_dept_sal ON employee (department_id, salary);
CREATE INDEX idx_proj_dept    ON project (department_id);
CREATE INDEX idx_proj_status  ON project (status);
CREATE INDEX idx_proj_dates   ON project (start_date, end_date);

-- DDL: Indexes (Hash) — 등호 조건 O(1) 검색
CREATE INDEX idx_emp_email  ON employee   (email) USING HASH;
CREATE INDEX idx_dept_code  ON department (code)  USING HASH;

-- DDL: Views
CREATE VIEW v_active_dept  AS SELECT id, code, name, budget, dept_type, headcount FROM department WHERE is_active = true;
CREATE VIEW v_dept_finance AS SELECT id, name, budget, annual_target, fiscal_year FROM department;
CREATE VIEW v_active_emp   AS SELECT id, first_name, last_name, email, department_id, job_title FROM employee WHERE termination_date IS NULL;
CREATE VIEW v_manager      AS SELECT id, first_name, last_name, department_id, salary, performance FROM employee WHERE is_manager = true;
CREATE VIEW v_senior_emp   AS SELECT id, first_name, last_name, experience_years, salary, skills FROM employee WHERE experience_years >= 7;
CREATE VIEW v_active_proj  AS SELECT id, code, name, status, priority, department_id FROM project WHERE status = 'active';
CREATE VIEW v_high_priority AS SELECT id, name, priority, budget, department_id FROM project WHERE priority <= 2;

-- DDL: Verify
SHOW TABLES;
DESCRIBE employee;
SHOW INDEX FROM project;
SHOW CREATE TABLE employee;
SHOW CREATE VIEW v_active_emp;
CREATE DATABASE IF NOT EXISTS company;
CREATE TABLE IF NOT EXISTS department (dummy INT);

-- INSERT: department (5 rows)
INSERT INTO department (code, name, budget, annual_target, headcount, floor_num, is_active, established, open_time, fiscal_year, description, metadata, dept_type, perks) VALUES
    ('ENG','Engineering',     5000000.00,10000000,20,3,true, '2010-01-15','09:00:00',2024,'Core product development','{"building":"A","room":3}','engineering','gym,cafe,parking'),
    ('MKT','Marketing',       1500000.00, 3000000, 8,2,true, '2012-06-01','08:30:00',2024,'Brand and growth',       '{"building":"B","room":2}','marketing',  'cafe,parking'),
    ('FIN','Finance',         1200000.00, 2000000, 5,4,true, '2011-03-10','09:00:00',2024,'Financial planning',     '{"building":"A","room":4}','finance',    'cafe,library'),
    ('HRS','Human Resources',  600000.00,  800000, 4,1,true, '2013-09-01','08:00:00',2024,'Talent management',      '{"building":"C","room":1}','hr',         'gym,cafe,childcare'),
    ('OPS','Operations',      2000000.00, 4000000,12,2,false,'2014-11-15','07:00:00',2023,'Cloud infrastructure',   '{"building":"D","room":2}','ops',        'parking');

-- INSERT: employee (12 rows)
INSERT INTO employee (employee_code,first_name,last_name,email,birth_date,hire_date,salary,hourly_rate,performance,department_id,manager_id,job_title,emp_type,skills,experience_years,is_manager,annual_leave,personal_data,bio) VALUES
    (1001,'Alice', 'Johnson', 'alice.j@co.com',  '1990-05-15','2018-03-01',120000.00, 0.0,9.5,1,NULL,'Sr Engineer',  'full_time','python,rust,sql,devops', 8,true, 25,'{"emergency":"Bob"}',    'Systems programmer'),
    (1002,'Bob',   'Smith',   'bob.s@co.com',    '1988-11-20','2016-07-15', 95000.00, 0.0,8.0,1,   1,'Backend Eng',  'full_time','java,sql,devops',        10,false,20,'{"emergency":"Alice"}', 'API specialist'),
    (1003,'Carol', 'Williams','carol.w@co.com',  '1993-02-28','2020-01-10', 85000.00, 0.0,7.5,2,NULL,'Mkt Manager',  'full_time','design,python',           5,true, 20,'{"emergency":"Dave"}',  'Brand expert'),
    (1004,'Dave',  'Brown',   'dave.b@co.com',   '1995-08-10','2021-06-01', 75000.00, 0.0,8.5,2,   3,'Content Spec', 'full_time','design',                  4,false,20,'{"emergency":"Carol"}','Strategist'),
    (1005,'Eve',   'Davis',   'eve.d@co.com',    '1985-12-01','2012-04-20',140000.00, 0.0,9.8,3,NULL,'Finance Dir',  'full_time','sql,ml',                 14,true, 30,'{"emergency":"Frank"}','Risk expert'),
    (1006,'Frank', 'Miller',  'frank.m@co.com',  '1992-07-25','2019-09-15', 78000.00, 0.0,7.0,3,   5,'Analyst',      'full_time','sql,python',              6,false,20,'{"emergency":"Eve"}',   'Quant analyst'),
    (1007,'Grace', 'Wilson',  'grace.w@co.com',  '1997-04-12','2022-02-01', 65000.00,55.0,8.2,4,NULL,'HR Spec',      'full_time','design',                  3,false,20,'{"emergency":"Henry"}', 'Talent acq'),
    (1008,'Henry', 'Moore',   'henry.m@co.com',  '1991-09-30','2017-11-01', 90000.00, 0.0,8.8,1,   1,'DevOps Eng',   'full_time','rust,devops,sql',         8,false,22,'{"emergency":"Grace"}', 'Infra specialist'),
    (1009,'Iris',  'Taylor',  'iris.t@co.com',   '1999-01-05','2023-08-15', 55000.00,30.0,7.8,1,   1,'Jr Developer', 'part_time','python,sql',              2,false,15,'{"emergency":"Jack"}',  'Full-stack dev'),
    (1010,'Jack',  'Anderson','jack.a@co.com',   '1994-06-18','2020-05-10', 70000.00, 0.0,6.5,NULL,NULL,'Consultant','contract', 'sql,ml',                  7,false,10,'{"emergency":"Iris"}',  'Data consultant'),
    (1011,'Karen', 'Lee',     'karen.l@co.com',  '1989-03-22','2015-08-01',110000.00, 0.0,9.2,1,NULL,'Lead Eng',     'full_time','python,rust,devops',      11,true, 25,'{"emergency":"Liam"}',  'Platform lead'),
    (1012,'Liam',  'Chen',    'liam.c@co.com',   '1996-11-14','2021-03-15', 80000.00, 0.0,7.9,2,   3,'Mkt Analyst',  'full_time','python,design',            4,false,20,'{"emergency":"Karen"}','Data marketer');

-- INSERT: project (5 rows)
INSERT INTO project (code,name,description,budget,alloc_hours,team_size,priority,start_date,end_date,deadline,revenue,completion,department_id,lead_id,status,tech_stack,is_public,contract_data) VALUES
    ('PRJ-001','Core Platform Rewrite','Legacy to Rust',      2000000.00,5000,8,1,'2024-01-01','2024-12-31','2025-01-01 00:00:00',8000000.0, 45.0,1,1, 'active',   'backend,database,cloud',   false,'{"client":"internal","type":"capex"}'),
    ('PRJ-002','AI Engine',            'ML recommendations',  1500000.00,3000,5,2,'2024-03-01','2024-09-30','2024-10-01 00:00:00',5000000.0, 70.0,1,11,'active',   'backend,ai,database',       false,'{"client":"internal","type":"opex"}'),
    ('PRJ-003','Brand Refresh',        'Identity overhaul',    500000.00,1000,4,2,'2024-02-15','2024-06-30','2024-07-01 00:00:00',      0.0,100.0,2,3, 'completed','frontend,ai',               true, '{"client":"external","type":"marketing"}'),
    ('PRJ-004','Finance Dashboard',    'Real-time reporting',  800000.00,2000,6,3,'2024-04-01',NULL,        '2024-11-01 00:00:00',2000000.0, 30.0,3,5, 'active',   'frontend,backend,database', false,'{"client":"internal","type":"capex"}'),
    ('PRJ-006','Mobile App MVP',       'iOS and Android',     1200000.00,4000,7,1,'2023-09-01','2024-05-31','2024-06-01 00:00:00',3000000.0,100.0,1,8, 'completed','mobile,backend,cloud',      true, '{"client":"external","type":"revenue"}');

-- SELECT
SELECT id, first_name, last_name, salary FROM employee WHERE salary >= 80000 AND emp_type = 'full_time' ORDER BY salary DESC;
SELECT first_name, last_name FROM employee ORDER BY hire_date LIMIT 3 OFFSET 1;
SELECT DISTINCT emp_type FROM employee ORDER BY emp_type;
SELECT first_name FROM employee WHERE department_id IN (1,2) AND salary BETWEEN 70000 AND 130000;
SELECT first_name, last_name FROM employee WHERE first_name LIKE 'A%' OR department_id IS NULL;
SELECT first_name FROM employee WHERE first_name REGEXP '^[AEI]';
SELECT first_name, REGEXP_LIKE(first_name,'^A') AS starts_a, REGEXP_REPLACE(email,'@co.com','') AS username FROM employee LIMIT 3;
SELECT name, budget FROM department WHERE NOT (budget < 1000000);
SELECT id, metadata->>'$.building' AS building, metadata->>'$.room' AS room FROM department ORDER BY id;
SELECT id, personal_data->>'$.emergency' AS emergency FROM employee LIMIT 4;
SELECT id, JSON_EXTRACT(contract_data,'$.type') AS contract_type, JSON_VALUE(contract_data,'$.client') AS client FROM project ORDER BY id LIMIT 4;

-- Aggregate
SELECT COUNT(*), SUM(salary), AVG(salary), MAX(salary), MIN(salary) FROM employee;
SELECT department_id, COUNT(*) AS headcount, AVG(salary) AS avg_salary FROM employee GROUP BY department_id HAVING headcount >= 2 ORDER BY avg_salary DESC;
SELECT emp_type, GROUP_CONCAT(first_name SEPARATOR ', ') AS names FROM employee GROUP BY emp_type ORDER BY emp_type;
SELECT COUNT(DISTINCT department_id), SUM(DISTINCT budget), STDDEV(budget), VARIANCE(budget) FROM project;
SELECT emp_type, COUNT(*) AS n, SUM(salary) AS total FROM employee GROUP BY emp_type ORDER BY total DESC;

-- JOIN
SELECT e.first_name, e.last_name, d.name AS dept_name, e.salary FROM employee e JOIN department d ON e.department_id = d.id ORDER BY e.salary DESC;
SELECT e.first_name, d.name AS dept, p.name AS project, p.status FROM employee e JOIN project p ON e.id = p.lead_id JOIN department d ON e.department_id = d.id ORDER BY e.first_name;
SELECT e.first_name, d.name AS dept_name FROM employee e LEFT JOIN department d ON e.department_id = d.id ORDER BY e.id;
SELECT d.name AS dept_name, p.name AS proj_name FROM department d RIGHT JOIN project p ON d.id = p.department_id ORDER BY p.id;
SELECT d.name, p.name AS project FROM department d FULL OUTER JOIN project p ON d.id = p.department_id ORDER BY d.name LIMIT 8;
SELECT d.name AS dept, e.first_name FROM department d CROSS JOIN employee e ORDER BY d.name, e.id LIMIT 6;

-- Subquery
SELECT first_name, salary FROM employee WHERE salary > (SELECT AVG(salary) FROM employee);
SELECT first_name, last_name FROM employee WHERE id IN (SELECT lead_id FROM project WHERE budget > 1000000);
SELECT first_name FROM employee WHERE EXISTS (SELECT 1 FROM project p WHERE p.lead_id = employee.id AND p.status = 'active');
SELECT status, avg_completion FROM (SELECT status, AVG(completion) AS avg_completion FROM project GROUP BY status) AS sub WHERE avg_completion > 30;
SELECT first_name, (SELECT MAX(salary) FROM employee) AS max_salary FROM employee ORDER BY salary DESC LIMIT 3;
SELECT name FROM project WHERE department_id NOT IN (SELECT id FROM department WHERE is_active = false);

-- UNION / INTERSECT / EXCEPT
SELECT first_name AS label, 'employee' AS entity FROM employee WHERE department_id = 1
UNION
SELECT name, 'project' AS entity FROM project WHERE department_id = 1
ORDER BY label;

SELECT lead_id AS person_id FROM project WHERE status = 'active'
UNION ALL
SELECT lead_id FROM project WHERE completion >= 50
ORDER BY person_id;

SELECT lead_id FROM project WHERE budget >= 1000000
INTERSECT
SELECT lead_id FROM project WHERE status = 'active';

SELECT id FROM employee
EXCEPT
SELECT lead_id FROM project WHERE lead_id IS NOT NULL;

-- CTE
WITH high_performer AS (
    SELECT id, first_name, last_name, salary, department_id FROM employee WHERE performance >= 9.0
)
SELECT hp.first_name, hp.salary, d.name AS department
FROM high_performer hp JOIN department d ON hp.department_id = d.id ORDER BY hp.salary DESC;

WITH dept_stats AS (
    SELECT department_id, COUNT(*) AS headcount, SUM(salary) AS total_salary FROM employee GROUP BY department_id
),
proj_count AS (
    SELECT department_id, COUNT(*) AS num_projects FROM project GROUP BY department_id
)
SELECT d.name, ds.headcount, ds.total_salary, pc.num_projects
FROM department d
LEFT JOIN dept_stats ds ON d.id = ds.department_id
LEFT JOIN proj_count pc ON d.id = pc.department_id
ORDER BY d.id;

WITH RECURSIVE mgmt_tree AS (
    SELECT id, first_name, last_name, manager_id, 0 AS depth
    FROM employee WHERE manager_id IS NULL
    UNION ALL
    SELECT e.id, e.first_name, e.last_name, e.manager_id, t.depth + 1
    FROM employee e JOIN mgmt_tree t ON e.manager_id = t.id
)
SELECT id, first_name, last_name, depth FROM mgmt_tree ORDER BY depth, id;

-- Window Functions
SELECT first_name, salary,
    ROW_NUMBER()   OVER (ORDER BY salary DESC)                                 AS overall_rank,
    RANK()         OVER (PARTITION BY department_id ORDER BY salary DESC)      AS dept_rank,
    DENSE_RANK()   OVER (PARTITION BY department_id ORDER BY salary DESC)      AS dept_dense,
    LAG(salary,1)  OVER (PARTITION BY department_id ORDER BY salary)           AS prev_salary,
    LEAD(salary,1) OVER (PARTITION BY department_id ORDER BY salary)           AS next_salary,
    FIRST_VALUE(salary) OVER (PARTITION BY department_id ORDER BY salary DESC) AS top_in_dept
FROM employee WHERE department_id IS NOT NULL ORDER BY department_id, salary DESC;

SELECT id, salary,
    SUM(salary)    OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)     AS running_total,
    AVG(salary)    OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)             AS moving_avg,
    NTH_VALUE(salary,2) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS second_val,
    NTILE(3)       OVER (ORDER BY salary)  AS bucket,
    PERCENT_RANK() OVER (ORDER BY salary)  AS pct_rank,
    CUME_DIST()    OVER (ORDER BY salary)  AS cume_d
FROM employee ORDER BY id;

SELECT department_id,
    SUM(budget)     OVER (PARTITION BY department_id) AS dept_budget_total,
    COUNT(*)        OVER (PARTITION BY department_id) AS dept_proj_count,
    MAX(completion) OVER (PARTITION BY department_id) AS max_completion
FROM project ORDER BY department_id;

-- Scalar Functions
SELECT UPPER(first_name), LOWER(last_name), LENGTH(email), CONCAT(first_name,' ',last_name) AS full_name FROM employee LIMIT 3;
SELECT SUBSTR(email,1,5) AS prefix, REPLACE(email,'@co.com','') AS username, LPAD(id,5,'0') AS padded FROM employee LIMIT 3;
SELECT LEFT(first_name,3) AS pfx, REVERSE(last_name) AS rev, REPEAT('-',3) AS sep, INSTR(email,'@') AS at_pos, ASCII('A') AS ascii_a, HEX(255) AS hex_val, FORMAT(salary,0) AS fmt_sal FROM employee LIMIT 3;
SELECT ROUND(3.14159,2), ABS(-9.99), CEIL(2.1), FLOOR(2.9), MOD(17,5), SQRT(144), POW(2,10), LOG2(1024), LOG10(1000), PI(), SIGN(-1), TRUNCATE(9.876,1), RAND() >= 0 AS rand_ok;
SELECT YEAR(hire_date), MONTH(hire_date), DAY(hire_date), DAYOFWEEK(hire_date), DATEDIFF('2026-05-01',hire_date) AS days_employed, DATE_FORMAT(hire_date,'%Y-%m') AS hire_month, DATE_ADD(hire_date, INTERVAL 1 YEAR) AS anniversary, TIMESTAMPDIFF(YEAR,hire_date,'2026-05-01') AS years_employed FROM employee LIMIT 3;
SELECT COALESCE(department_id,-1) AS dept_or_default, IFNULL(department_id,-1) AS dept_ifnull, NULLIF(emp_type,'contract') AS nullif_contract, GREATEST(1,5,3), LEAST(1,5,3), CAST(experience_years AS INT) AS exp_int, IF(salary>100000,'Senior','Standard') AS emp_level, CASE WHEN salary >= 120000 THEN 'Senior' WHEN salary >= 80000 THEN 'Mid' ELSE 'Junior' END AS grade, MD5(email), LENGTH(UUID()) > 0 AS uuid_ok FROM employee LIMIT 3;

-- INSERT Variants
INSERT IGNORE INTO department (code, name, budget, dept_type) VALUES ('ENG', 'Engineering Dup', 0, 'engineering');

INSERT INTO employee (employee_code,first_name,last_name,email,hire_date,salary,department_id)
    VALUES (1001,'Alice','Johnson','alice.j@co.com','2018-03-01',130000,1)
    ON DUPLICATE KEY UPDATE salary=130000;
SELECT id, first_name, salary FROM employee WHERE employee_code=1001;
UPDATE employee SET salary=120000 WHERE employee_code=1001;

CREATE TABLE project_backup (id INT PRIMARY KEY, code VARCHAR(20), name VARCHAR(200), budget DECIMAL(15,2));
INSERT INTO project_backup SELECT id, code, name, budget FROM project WHERE status='completed';
SELECT * FROM project_backup ORDER BY id;
TRUNCATE TABLE project_backup;
DROP TABLE project_backup;

-- RETURNING
INSERT INTO department (code,name,budget,dept_type) VALUES ('TMP','Temporary',0,'hr') RETURNING id, code, name;
DELETE FROM department WHERE code='TMP' RETURNING id, name;
UPDATE employee SET salary=salary+1000 WHERE id=1 RETURNING id, first_name, salary;
UPDATE employee SET salary=salary-1000 WHERE id=1;

-- Multi-table DML
UPDATE employee e, department d SET e.salary=e.salary+500, d.headcount=d.headcount+1
    WHERE e.department_id=d.id AND d.code='ENG';
UPDATE employee SET salary=salary-500 WHERE department_id=1;
DELETE project FROM project
    JOIN department ON project.department_id=department.id
    WHERE project.status='cancelled';

-- ALTER TABLE
ALTER TABLE employee ADD COLUMN profile_url VARCHAR(200);
UPDATE employee SET profile_url=CONCAT('linkedin.com/',LOWER(first_name)) WHERE id<=3;
ALTER TABLE employee RENAME COLUMN profile_url TO linkedin_url;
SELECT id, first_name, linkedin_url FROM employee WHERE linkedin_url IS NOT NULL ORDER BY id;
ALTER TABLE employee DROP COLUMN linkedin_url;
ALTER TABLE employee MODIFY COLUMN job_title VARCHAR(150) DEFAULT 'Staff';
ALTER TABLE project ADD CONSTRAINT uq_proj_name UNIQUE (name);
ALTER TABLE project DROP CONSTRAINT uq_proj_name;
ALTER TABLE project ADD CONSTRAINT chk_team_size CHECK (team_size >= 1);
ALTER TABLE project DROP CONSTRAINT chk_team_size;

-- ENUM / SET validation
INSERT INTO employee (employee_code,first_name,last_name,email,hire_date,salary,department_id,emp_type)
    VALUES (9999,'Bad','Type','bad@co.com','2024-01-01',50000,1,'freelance');
INSERT INTO department (code,name,budget,dept_type,perks)
    VALUES ('BAD','Bad Dept',0,'engineering','sauna');
SELECT code, dept_type, perks FROM department ORDER BY id LIMIT 3;

-- FK behavior
DELETE FROM department WHERE code='ENG';

INSERT INTO department (code,name,budget,dept_type) VALUES ('DEL','ToDelete',1000.00,'hr');
INSERT INTO employee (employee_code,first_name,last_name,email,hire_date,salary,department_id)
    SELECT 8888,'Delete','Test','delete.test@co.com','2024-01-01',60000,id FROM department WHERE code='DEL';
SELECT id, first_name, department_id FROM employee WHERE employee_code=8888;
DELETE FROM department WHERE code='DEL';
SELECT id, first_name, department_id FROM employee WHERE employee_code=8888;
DELETE FROM employee WHERE employee_code=8888;

INSERT INTO employee (employee_code,first_name,last_name,email,hire_date,salary,department_id)
    VALUES (7777,'Cascade','Test','cascade.test@co.com','2024-01-01',60000,1);
INSERT INTO project (code,name,budget,department_id,lead_id,status)
    SELECT 'TMP-CAS','Cascade Test',0,1,id,'planning' FROM employee WHERE employee_code=7777;
SELECT COUNT(*) AS proj_before FROM project WHERE code='TMP-CAS';
DELETE FROM employee WHERE employee_code=7777;
SELECT COUNT(*) AS proj_after FROM project WHERE code='TMP-CAS';

-- MERGE
INSERT INTO department (code,name,budget,dept_type) VALUES ('TMP','Temp Dept',0.00,'hr');
CREATE TABLE dept_upd (code VARCHAR(10) PRIMARY KEY, name VARCHAR(100), budget DECIMAL(15,2), dept_type ENUM('engineering','sales','marketing','finance','hr','ops','legal'));
INSERT INTO dept_upd VALUES
    ('ENG','Engineering Pro',6000000.00,'engineering'),
    ('TMP','Temp Closed',    0.00,'hr'),
    ('NEW','New Division',   500000.00,'sales');
MERGE INTO department USING dept_upd ON department.code=dept_upd.code
    WHEN MATCHED AND dept_upd.budget=0.00 THEN DELETE
    WHEN MATCHED THEN UPDATE SET budget=dept_upd.budget
    WHEN NOT MATCHED THEN INSERT (code,name,budget) VALUES (dept_upd.code,dept_upd.name,dept_upd.budget);
SELECT code, name, budget FROM department ORDER BY id;
DROP TABLE dept_upd;

-- Stored Procedures
CREATE PROCEDURE classify_salary(IN p_salary INT) BEGIN DECLARE grade VARCHAR(20) DEFAULT 'standard'; IF p_salary >= 120000 THEN SET grade = 'senior'; ELSEIF p_salary >= 80000 THEN SET grade = 'mid'; END IF; SELECT grade AS salary_grade; END;
CALL classify_salary(130000);
CALL classify_salary(85000);
CALL classify_salary(50000);
DROP PROCEDURE classify_salary;

CREATE PROCEDURE sum_to_n(IN n INT) BEGIN DECLARE i INT DEFAULT 1; DECLARE total INT DEFAULT 0; WHILE i <= n DO SET total = total + i; SET i = i + 1; END WHILE; SELECT total AS result; END;
CALL sum_to_n(10);
DROP PROCEDURE sum_to_n;

CREATE PROCEDURE odd_sum(IN n INT) BEGIN DECLARE i INT DEFAULT 0; DECLARE total INT DEFAULT 0; calc: LOOP SET i = i + 1; IF i > n THEN LEAVE calc; END IF; IF MOD(i,2) = 0 THEN ITERATE calc; END IF; SET total = total + i; END LOOP; SELECT total AS odd_sum; END;
CALL odd_sum(10);
DROP PROCEDURE odd_sum;

CREATE PROCEDURE countdown(IN start_val INT) BEGIN DECLARE counter INT; SET counter = start_val; REPEAT SET counter = counter - 1; UNTIL counter <= 0 END REPEAT; SELECT counter AS final; END;
CALL countdown(5);
DROP PROCEDURE countdown;

CREATE PROCEDURE double_val(IN x INT) BEGIN DECLARE result INT; SET result = x * 2; SELECT result AS doubled; END;
CALL double_val(21);
DROP PROCEDURE double_val;

-- PREPARE / EXECUTE
PREPARE find_emp FROM 'SELECT id, first_name, salary FROM employee WHERE id = ?';
SET @eid = 3;
EXECUTE find_emp USING @eid;
SET @eid = 5;
EXECUTE find_emp USING @eid;
DEALLOCATE PREPARE find_emp;
SET @cutoff = 9.0;
SELECT first_name, performance FROM employee WHERE performance >= 9.0 ORDER BY performance DESC;

-- Triggers
CREATE TRIGGER trg_after_insert_emp AFTER INSERT ON employee FOR EACH ROW
    UPDATE department SET headcount = headcount + 1 WHERE id = 1;
INSERT INTO employee (employee_code,first_name,last_name,email,hire_date,salary,department_id)
    VALUES (6666,'Trigger','Test','trigger.test@co.com','2024-01-01',60000,2);
SELECT headcount FROM department WHERE id=1;
DELETE FROM employee WHERE employee_code=6666;
DROP TRIGGER IF EXISTS trg_after_insert_emp;

CREATE TRIGGER trg_before_update_proj BEFORE UPDATE ON project FOR EACH ROW
    UPDATE department SET is_active=true WHERE id=1;
UPDATE project SET budget=budget+1 WHERE code='PRJ-004';
SELECT is_active FROM department WHERE id=1;
UPDATE project SET budget=budget-1 WHERE code='PRJ-004';
DROP TRIGGER IF EXISTS trg_before_update_proj;

CREATE TRIGGER trg_after_delete_proj AFTER DELETE ON project FOR EACH ROW
    UPDATE department SET headcount=headcount-1 WHERE id=1;
DELETE FROM project WHERE code='PRJ-003';
SELECT headcount FROM department WHERE id=1;
DROP TRIGGER IF EXISTS trg_after_delete_proj;
INSERT INTO project (code,name,budget,department_id,lead_id,status,tech_stack)
    VALUES ('PRJ-003','Brand Refresh',500000.00,2,3,'completed','frontend,ai');

-- Transactions
BEGIN;
INSERT INTO employee (employee_code,first_name,last_name,email,hire_date,salary,department_id)
    VALUES (5555,'Txn','Test','txn.test@co.com','2024-01-01',70000,1);
SAVEPOINT sp_insert;
UPDATE employee SET salary=999999 WHERE employee_code=5555;
SELECT salary FROM employee WHERE employee_code=5555;
ROLLBACK TO SAVEPOINT sp_insert;
SELECT salary FROM employee WHERE employee_code=5555;
COMMIT;
SELECT employee_code, first_name, salary FROM employee WHERE employee_code=5555;
DELETE FROM employee WHERE employee_code=5555;

BEGIN;
UPDATE department SET budget=0 WHERE code='ENG';
ROLLBACK;
SELECT budget FROM department WHERE code='ENG';

BEGIN;
SAVEPOINT sp_check;
RELEASE SAVEPOINT sp_check;
COMMIT;

-- Isolation Level
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL REPEATABLE READ;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ UNCOMMITTED;
SET ISOLATION LEVEL READ COMMITTED;

-- Locking
BEGIN;
SELECT id, first_name, salary FROM employee WHERE id=1 FOR UPDATE;
SHOW LOCKS;
COMMIT;

BEGIN;
SELECT id, name FROM department WHERE id=1 FOR SHARE;
SELECT id, name FROM department WHERE id=2 FOR SHARE;
SHOW LOCKS;
COMMIT;

-- EXPLAIN / ANALYZE (B-tree + Hash Index)
EXPLAIN SELECT * FROM employee WHERE id=1;
EXPLAIN SELECT * FROM employee WHERE department_id=1;
-- Hash Index Scan: 등호 조건에서 idx_emp_email / idx_dept_code 자동 선택
EXPLAIN SELECT * FROM employee WHERE email = 'alice.j@co.com';
SELECT id, first_name, email FROM employee WHERE email = 'alice.j@co.com';
EXPLAIN SELECT id, name FROM department WHERE code = 'ENG';
SELECT id, name FROM department WHERE code = 'ENG';
EXPLAIN SELECT e.first_name, d.name, p.name FROM employee e
    JOIN department d ON e.department_id=d.id
    JOIN project p ON d.id=p.department_id;
ANALYZE TABLE department;
ANALYZE TABLE employee;
EXPLAIN ANALYZE SELECT * FROM employee WHERE department_id=1 AND salary >= 80000;

-- Views: Usage
SELECT * FROM v_active_dept ORDER BY id;
SELECT * FROM v_manager ORDER BY salary DESC;
SELECT * FROM v_active_proj ORDER BY priority;
UPDATE v_active_emp SET job_title='Principal Engineer' WHERE id=1;
SELECT id, first_name, job_title FROM employee WHERE id=1;
UPDATE employee SET job_title='Sr Engineer' WHERE id=1;

-- UDF
CREATE FUNCTION monthly_salary(annual_salary) RETURNS DECIMAL RETURN annual_salary / 12;
SELECT first_name, salary, monthly_salary(salary) AS monthly FROM employee ORDER BY salary DESC LIMIT 4;
DROP FUNCTION monthly_salary;

CREATE FUNCTION perf_label(score) RETURNS TEXT RETURN CONCAT('score=', score);
SELECT first_name, performance, perf_label(performance) AS label FROM employee ORDER BY performance DESC LIMIT 5;
DROP FUNCTION perf_label;

-- INFORMATION_SCHEMA
SELECT table_name, table_rows FROM information_schema.tables WHERE table_schema='company' ORDER BY table_name;
SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name='employee' ORDER BY ordinal_position LIMIT 8;
SELECT constraint_name, table_name, constraint_type FROM information_schema.table_constraints WHERE table_schema='company' ORDER BY table_name, constraint_name LIMIT 10;

-- FETCH FIRST
SELECT id, first_name, salary FROM employee ORDER BY salary DESC FETCH FIRST 3 ROWS ONLY;
SELECT id, first_name, salary FROM employee ORDER BY salary ASC FETCH NEXT 3 ROWS ONLY;

-- JOIN USING / NATURAL JOIN
CREATE TABLE tmp_dept (dept_id INT PRIMARY KEY, dept_name VARCHAR(50) NOT NULL);
CREATE TABLE tmp_emp  (emp_name VARCHAR(50) NOT NULL, dept_id INT NOT NULL);
INSERT INTO tmp_dept VALUES (1,'Engineering'),(2,'Marketing'),(3,'Finance');
INSERT INTO tmp_emp  VALUES ('Alice',1),('Bob',2),('Carol',3),('Dave',1);
SELECT dept_id, dept_name, emp_name FROM tmp_dept JOIN tmp_emp USING (dept_id) ORDER BY dept_id, emp_name;
SELECT dept_name, emp_name FROM tmp_dept NATURAL JOIN tmp_emp ORDER BY dept_name;
DROP TABLE tmp_emp;
DROP TABLE tmp_dept;

-- DCL: Users
CREATE USER 'testuser'@'%' IDENTIFIED BY 'secure_pass_123';
GRANT SELECT, INSERT, UPDATE ON company.employee TO 'testuser'@'%' WITH GRANT OPTION;
GRANT SELECT ON company.department TO 'testuser'@'%';
SHOW GRANTS FOR 'testuser'@'%';
REVOKE UPDATE ON company.employee FROM 'testuser'@'%';
SHOW GRANTS FOR 'testuser'@'%';

-- Roles
CREATE ROLE analyst;
CREATE ROLE developer;
CREATE ROLE administrator;
SHOW ROLES;
GRANT ROLE analyst TO 'testuser'@'%';
GRANT ROLE developer TO 'testuser'@'%' WITH ADMIN OPTION;
REVOKE ROLE analyst FROM 'testuser'@'%';
DROP ROLE analyst;
DROP ROLE IF EXISTS developer;
DROP ROLE IF EXISTS administrator;
SHOW ROLES;

-- Synonyms
CREATE SYNONYM emp_list FOR employee;
CREATE OR REPLACE SYNONYM emp_list FOR employee;
SHOW SYNONYMS;
SELECT id, first_name, last_name FROM emp_list ORDER BY id LIMIT 3;
CREATE SYNONYM proj_list FOR project;
SELECT code, name, status FROM proj_list ORDER BY id LIMIT 3;
DROP SYNONYM emp_list;
DROP SYNONYM IF EXISTS proj_list;
SHOW SYNONYMS;

-- Monitoring
CHECKPOINT;
VACUUM;
VACUUM employee;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
SHOW PROCESSLIST;
SHOW DATABASES;

-- Backup
BACKUP DATABASE company INTO 'company_backup.sql';

-- Clean up
DROP USER IF EXISTS 'testuser'@'%';
DROP VIEW IF EXISTS v_active_dept;
DROP VIEW IF EXISTS v_dept_finance;
DROP VIEW IF EXISTS v_active_emp;
DROP VIEW IF EXISTS v_manager;
DROP VIEW IF EXISTS v_senior_emp;
DROP VIEW IF EXISTS v_active_proj;
DROP VIEW IF EXISTS v_high_priority;
DROP INDEX IF EXISTS idx_dept_type;
DROP INDEX IF EXISTS idx_dept_active;
DROP INDEX IF EXISTS idx_emp_dept;
DROP INDEX IF EXISTS idx_emp_type;
DROP INDEX IF EXISTS idx_emp_dept_sal;
DROP INDEX IF EXISTS idx_proj_dept;
DROP INDEX IF EXISTS idx_proj_status;
DROP INDEX IF EXISTS idx_proj_dates;
DROP INDEX IF EXISTS idx_emp_email;
DROP INDEX IF EXISTS idx_dept_code;
DROP TABLE IF EXISTS project;
DROP TABLE IF EXISTS employee;
DROP TABLE IF EXISTS department;
DROP DATABASE company;
SHOW DATABASES;
