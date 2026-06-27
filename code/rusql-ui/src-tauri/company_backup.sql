-- RuSQL backup of database `company`
-- Generated: 2026-07-10

CREATE DATABASE IF NOT EXISTS `company`;
USE `company`;

DROP TABLE IF EXISTS `department`;
CREATE TABLE `department` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `code` VARCHAR(10) NOT NULL,
  `name` VARCHAR(100) NOT NULL,
  `budget` DECIMAL(15,2) DEFAULT '0.00',
  `annual_target` BIGINT DEFAULT '0',
  `headcount` SMALLINT DEFAULT '0',
  `floor_num` TINYINT DEFAULT '1',
  `is_active` BOOLEAN DEFAULT 'true',
  `established` DATE,
  `open_time` TIME,
  `fiscal_year` YEAR,
  `description` TEXT,
  `metadata` JSON,
  `dept_type` ENUM('engineering','sales','marketing','finance','hr','ops','legal'),
  `perks` SET('gym','cafe','parking','library','childcare'),
  `contact_email` VARCHAR(100),
  `location` VARCHAR(100),
  `created_at` TIMESTAMP,
  PRIMARY KEY (`id`)
);

INSERT INTO `department` (`description`, `floor_num`, `metadata`, `fiscal_year`, `name`, `headcount`, `open_time`, `contact_email`, `created_at`, `annual_target`, `budget`, `established`, `perks`, `location`, `is_active`, `dept_type`, `id`, `code`) VALUES ('Core product development', '3', '{"building":"A","room":3}', '2024', 'Engineering', '21', '09:00:00', 'eng@co.com', '2010-01-15 09:00:00', '10000000', '6000000.00', '2010-01-15', 'gym,cafe,parking', 'Building A 3F', 'true', 'engineering', '1', 'ENG');
INSERT INTO `department` (`description`, `floor_num`, `metadata`, `fiscal_year`, `name`, `headcount`, `open_time`, `contact_email`, `created_at`, `annual_target`, `budget`, `established`, `perks`, `location`, `is_active`, `dept_type`, `id`, `code`) VALUES ('Brand and growth', '2', '{"building":"B","room":2}', '2024', 'Marketing', '8', '08:30:00', 'mkt@co.com', '2012-06-01 08:30:00', '3000000', '1500000.00', '2012-06-01', 'cafe,parking', 'Building B 2F', 'true', 'marketing', '2', 'MKT');
INSERT INTO `department` (`description`, `floor_num`, `metadata`, `fiscal_year`, `name`, `headcount`, `open_time`, `contact_email`, `created_at`, `annual_target`, `budget`, `established`, `perks`, `location`, `is_active`, `dept_type`, `id`, `code`) VALUES ('Financial planning', '4', '{"building":"A","room":4}', '2024', 'Finance', '5', '09:00:00', 'fin@co.com', '2011-03-10 09:00:00', '2000000', '1200000.00', '2011-03-10', 'cafe,library', 'Building A 4F', 'true', 'finance', '3', 'FIN');
INSERT INTO `department` (`description`, `floor_num`, `metadata`, `fiscal_year`, `name`, `headcount`, `open_time`, `contact_email`, `created_at`, `annual_target`, `budget`, `established`, `perks`, `location`, `is_active`, `dept_type`, `id`, `code`) VALUES ('Talent management', '1', '{"building":"C","room":1}', '2024', 'Human Resources', '4', '08:00:00', 'hr@co.com', '2013-09-01 08:00:00', '800000', '600000.00', '2013-09-01', 'gym,cafe,childcare', 'Building C 1F', 'true', 'hr', '4', 'HRS');
INSERT INTO `department` (`description`, `floor_num`, `metadata`, `fiscal_year`, `name`, `headcount`, `open_time`, `contact_email`, `created_at`, `annual_target`, `budget`, `established`, `perks`, `location`, `is_active`, `dept_type`, `id`, `code`) VALUES ('Cloud infrastructure', '2', '{"building":"D","room":2}', '2023', 'Operations', '12', '07:00:00', 'ops@co.com', '2014-11-15 07:00:00', '4000000', '2000000.00', '2014-11-15', 'parking', 'Building D 2F', 'false', 'ops', '5', 'OPS');
INSERT INTO `department` (`description`, `floor_num`, `metadata`, `fiscal_year`, `name`, `headcount`, `open_time`, `contact_email`, `created_at`, `annual_target`, `budget`, `established`, `perks`, `location`, `is_active`, `dept_type`, `id`, `code`) VALUES (NULL, '1', NULL, NULL, 'New Division', '0', NULL, NULL, NULL, '0', '500000.00', NULL, NULL, NULL, 'true', NULL, '10', 'NEW');

DROP TABLE IF EXISTS `employee`;
CREATE TABLE `employee` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `employee_code` BIGINT NOT NULL,
  `first_name` VARCHAR(50) NOT NULL,
  `last_name` VARCHAR(50) NOT NULL,
  `email` VARCHAR(100) NOT NULL,
  `birth_date` DATE,
  `hire_date` DATE NOT NULL,
  `termination_date` DATETIME,
  `salary` DECIMAL(12,2),
  `performance` DOUBLE DEFAULT '0.0',
  `department_id` INT,
  `manager_id` INT,
  `job_title` VARCHAR(150) DEFAULT 'Staff',
  `emp_type` ENUM('full_time','part_time','contract','intern') DEFAULT 'full_time',
  `skills` SET('python','java','rust','sql','ml','devops','design'),
  `experience_years` TINYINT DEFAULT '0',
  `is_manager` BOOLEAN DEFAULT 'false',
  `annual_leave` SMALLINT DEFAULT '20',
  `personal_data` JSON,
  `bio` TEXT,
  PRIMARY KEY (`id`),
  FOREIGN KEY (`department_id`) REFERENCES `department`(`id`) ON DELETE SET NULL ON UPDATE CASCADE
);

INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('9.5', 'python,rust,sql,devops', '8', '25', NULL, 'Alice', 'Sr Engineer', '1990-05-15', '1001', 'full_time', '120000', 'Johnson', 'alice.j@co.com', '2018-03-01', NULL, 'Systems programmer', '1', '1', '{"emergency":"Bob"}', 'true');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('8.0', 'java,sql,devops', '10', '20', NULL, 'Bob', 'Backend Eng', '1988-11-20', '1002', 'full_time', '95000', 'Smith', 'bob.s@co.com', '2016-07-15', '1', 'API specialist', '2', '1', '{"emergency":"Alice"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('7.5', 'design,python', '5', '20', NULL, 'Carol', 'Mkt Manager', '1993-02-28', '1003', 'full_time', '85000.00', 'Williams', 'carol.w@co.com', '2020-01-10', NULL, 'Brand expert', '3', '2', '{"emergency":"Dave"}', 'true');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('8.5', 'design', '4', '20', NULL, 'Dave', 'Content Spec', '1995-08-10', '1004', 'full_time', '75000.00', 'Brown', 'dave.b@co.com', '2021-06-01', '3', 'Strategist', '4', '2', '{"emergency":"Carol"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('9.8', 'sql,ml', '14', '30', NULL, 'Eve', 'Finance Dir', '1985-12-01', '1005', 'full_time', '140000.00', 'Davis', 'eve.d@co.com', '2012-04-20', NULL, 'Risk expert', '5', '3', '{"emergency":"Frank"}', 'true');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('7.0', 'sql,python', '6', '20', NULL, 'Frank', 'Analyst', '1992-07-25', '1006', 'full_time', '78000.00', 'Miller', 'frank.m@co.com', '2019-09-15', '5', 'Quant analyst', '6', '3', '{"emergency":"Eve"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('8.2', 'design', '3', '20', NULL, 'Grace', 'HR Spec', '1997-04-12', '1007', 'full_time', '65000.00', 'Wilson', 'grace.w@co.com', '2022-02-01', NULL, 'Talent acq', '7', '4', '{"emergency":"Henry"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('8.8', 'rust,devops,sql', '8', '22', NULL, 'Henry', 'DevOps Eng', '1991-09-30', '1008', 'full_time', '90000', 'Moore', 'henry.m@co.com', '2017-11-01', '1', 'Infra specialist', '8', '1', '{"emergency":"Grace"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('7.8', 'python,sql', '2', '15', NULL, 'Iris', 'Jr Developer', '1999-01-05', '1009', 'part_time', '55000', 'Taylor', 'iris.t@co.com', '2023-08-15', '1', 'Full-stack dev', '9', '1', '{"emergency":"Jack"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('6.5', 'sql,ml', '7', '10', NULL, 'Jack', 'Consultant', '1994-06-18', '1010', 'contract', '70000.00', 'Anderson', 'jack.a@co.com', '2020-05-10', NULL, 'Data consultant', '10', NULL, '{"emergency":"Iris"}', 'false');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('9.2', 'python,rust,devops', '11', '25', NULL, 'Karen', 'Lead Eng', '1989-03-22', '1011', 'full_time', '110000', 'Lee', 'karen.l@co.com', '2015-08-01', NULL, 'Platform lead', '11', '1', '{"emergency":"Liam"}', 'true');
INSERT INTO `employee` (`performance`, `skills`, `experience_years`, `annual_leave`, `termination_date`, `first_name`, `job_title`, `birth_date`, `employee_code`, `emp_type`, `salary`, `last_name`, `email`, `hire_date`, `manager_id`, `bio`, `id`, `department_id`, `personal_data`, `is_manager`) VALUES ('7.9', 'python,design', '4', '20', NULL, 'Liam', 'Mkt Analyst', '1996-11-14', '1012', 'full_time', '80000.00', 'Chen', 'liam.c@co.com', '2021-03-15', '3', 'Data marketer', '12', '2', '{"emergency":"Karen"}', 'false');

DROP TABLE IF EXISTS `project`;
CREATE TABLE `project` (
  `id` INT NOT NULL AUTO_INCREMENT,
  `code` VARCHAR(20) NOT NULL,
  `name` VARCHAR(200) NOT NULL,
  `description` TEXT,
  `budget` DECIMAL(15,2) DEFAULT '0.00',
  `team_size` SMALLINT DEFAULT '1',
  `priority` TINYINT DEFAULT '3',
  `start_date` DATE,
  `end_date` DATE,
  `deadline` DATETIME,
  `updated_at` TIMESTAMP,
  `revenue` FLOAT DEFAULT '0.0',
  `completion` DOUBLE DEFAULT '0.0',
  `department_id` INT NOT NULL,
  `lead_id` INT,
  `status` ENUM('planning','active','on_hold','completed','cancelled') DEFAULT 'planning',
  `tech_stack` SET('frontend','backend','database','mobile','cloud','ai','security'),
  `is_public` BOOLEAN DEFAULT 'false',
  `contract_data` JSON,
  PRIMARY KEY (`id`),
  FOREIGN KEY (`department_id`) REFERENCES `department`(`id`) ON DELETE RESTRICT ON UPDATE CASCADE,
  FOREIGN KEY (`lead_id`) REFERENCES `employee`(`id`) ON DELETE CASCADE ON UPDATE CASCADE
);

INSERT INTO `project` (`start_date`, `budget`, `deadline`, `revenue`, `code`, `contract_data`, `priority`, `description`, `id`, `department_id`, `tech_stack`, `updated_at`, `completion`, `is_public`, `team_size`, `name`, `lead_id`, `end_date`, `status`) VALUES ('2024-01-01', '2000000.00', '2025-01-01 00:00:00', '8000000.0', 'PRJ-001', '{"client":"internal","type":"capex"}', '1', 'Legacy to Rust', '1', '1', 'backend,database,cloud', '2026-06-28 05:51:06', '45.0', 'false', '8', 'Core Platform Rewrite', '1', '2024-12-31', 'active');
INSERT INTO `project` (`start_date`, `budget`, `deadline`, `revenue`, `code`, `contract_data`, `priority`, `description`, `id`, `department_id`, `tech_stack`, `updated_at`, `completion`, `is_public`, `team_size`, `name`, `lead_id`, `end_date`, `status`) VALUES ('2024-03-01', '1500000.00', '2024-10-01 00:00:00', '5000000.0', 'PRJ-002', '{"client":"internal","type":"opex"}', '2', 'ML recommendations', '2', '1', 'backend,ai,database', '2026-06-28 05:51:06', '70.0', 'false', '5', 'AI Engine', '11', '2024-09-30', 'active');
INSERT INTO `project` (`start_date`, `budget`, `deadline`, `revenue`, `code`, `contract_data`, `priority`, `description`, `id`, `department_id`, `tech_stack`, `updated_at`, `completion`, `is_public`, `team_size`, `name`, `lead_id`, `end_date`, `status`) VALUES ('2024-04-01', '800000', '2024-11-01 00:00:00', '2000000.0', 'PRJ-004', '{"client":"internal","type":"capex"}', '3', 'Real-time reporting', '4', '3', 'frontend,backend,database', '2026-06-28 05:51:06', '30.0', 'false', '6', 'Finance Dashboard', '5', NULL, 'active');
INSERT INTO `project` (`start_date`, `budget`, `deadline`, `revenue`, `code`, `contract_data`, `priority`, `description`, `id`, `department_id`, `tech_stack`, `updated_at`, `completion`, `is_public`, `team_size`, `name`, `lead_id`, `end_date`, `status`) VALUES ('2023-09-01', '1200000.00', '2024-06-01 00:00:00', '3000000.0', 'PRJ-006', '{"client":"external","type":"revenue"}', '1', 'iOS and Android', '5', '1', 'mobile,backend,cloud', '2026-06-28 05:51:06', '100.0', 'true', '7', 'Mobile App MVP', '8', '2024-05-31', 'completed');
INSERT INTO `project` (`start_date`, `budget`, `deadline`, `revenue`, `code`, `contract_data`, `priority`, `description`, `id`, `department_id`, `tech_stack`, `updated_at`, `completion`, `is_public`, `team_size`, `name`, `lead_id`, `end_date`, `status`) VALUES (NULL, '500000.00', NULL, '0.0', 'PRJ-003', NULL, '3', NULL, '7', '2', 'frontend,ai', '2026-06-28 05:51:06', '0.0', 'false', '1', 'Brand Refresh', '3', NULL, 'completed');

