/* ============================================================ */
/* SECTION 0 : reset & bootstrap                                  */
/* ============================================================ */
DROP USER IF EXISTS 'storeuser'@'%';
DROP DATABASE IF EXISTS boutique;
CREATE DATABASE boutique;
USE boutique;

/* ============================================================ */
/* SECTION 1 : catalog tables -- written DBA-style, aligned       */
/* ============================================================ */
CREATE TABLE category (
    id             INT           AUTO_INCREMENT,
    parent_id      INT           REFERENCES category(id) ON DELETE SET NULL ON UPDATE CASCADE,
    name           VARCHAR(80)   NOT NULL,
    slug           VARCHAR(80)   NOT NULL,
    is_active      BOOLEAN       DEFAULT TRUE,
    display_order  SMALLINT      DEFAULT 0,
    created_at     TIMESTAMP,
    CONSTRAINT pk_category PRIMARY KEY (id),
    UNIQUE KEY uq_category_slug (slug)
);

CREATE TABLE supplier (
    id               INT            AUTO_INCREMENT,
    name             VARCHAR(120)   NOT NULL,
    country          VARCHAR(60),
    rating           DOUBLE         DEFAULT 0.0 CHECK (rating BETWEEN 0 AND 5),
    certifications   SET('iso9001','fairtrade','organic','cruelty_free'),
    contact          JSON,
    onboarded        DATE,
    contract_expiry  DATE,
    is_preferred     BOOLEAN        DEFAULT FALSE,
    CONSTRAINT pk_supplier PRIMARY KEY (id)
);

/* ---- product catalog: a different author, terser style ---- */
create table product (
  id INT AUTO_INCREMENT,
  sku VARCHAR(30) NOT NULL,
  name VARCHAR(150) NOT NULL,
  category_id INT REFERENCES category(id) ON DELETE SET NULL ON UPDATE CASCADE,
  supplier_id INT REFERENCES supplier(id) ON DELETE RESTRICT ON UPDATE CASCADE,
  unit_price DECIMAL(10,2) CHECK (unit_price >= 0),
  cost_price DECIMAL(10,2),
  weight_kg FLOAT,
  stock_qty INT DEFAULT 0,
  reorder_level SMALLINT DEFAULT 10,
  status ENUM('draft','active','discontinued','backordered') DEFAULT 'draft',
  tags SET('sale','new','featured','clearance','limited'),
  attributes JSON,
  thumbnail BLOB,
  launched_at DATE,
  last_restocked TIMESTAMP,
  is_taxable BOOLEAN DEFAULT TRUE,
  constraint pk_product primary key (id),
  unique key uq_product_sku (sku)
);

CREATE TABLE warehouse (
    id              INT         AUTO_INCREMENT,
    code            CHAR(4)     NOT NULL,
    city            VARCHAR(80),
    capacity_units  BIGINT,
    opened_year     YEAR,
    opening_time    TIME,
    manager_name    VARCHAR(80),
    is_operational  BOOL        DEFAULT TRUE,
    CONSTRAINT pk_warehouse PRIMARY KEY (id),
    UNIQUE KEY uq_warehouse_code (code)
);

-- inventory: composite primary key, quick junior-dev style
CREATE TABLE inventory (
    product_id INT NOT NULL REFERENCES product(id) ON DELETE CASCADE ON UPDATE CASCADE,
    warehouse_id INT NOT NULL REFERENCES warehouse(id) ON DELETE CASCADE ON UPDATE CASCADE,
    quantity INT DEFAULT 0 CHECK (quantity >= 0),
    last_counted DATETIME,
    PRIMARY KEY (product_id, warehouse_id)
);

CREATE TABLE customer (
    id               INT            AUTO_INCREMENT,
    referred_by      INT            REFERENCES customer(id) ON DELETE SET NULL ON UPDATE CASCADE,
    full_name        VARCHAR(120)   NOT NULL,
    email            VARCHAR(120)   NOT NULL,
    phone            VARCHAR(30),
    birth_date       DATE,
    signup_at        TIMESTAMP,
    tier             ENUM('bronze','silver','gold','platinum') DEFAULT 'bronze',
    interests        SET('electronics','fashion','home','sports','books','beauty'),
    loyalty_points   INT            DEFAULT 0,
    preferences      JSON,
    is_vip           BOOLEAN        DEFAULT FALSE,
    credit_limit     NUMERIC(10,2)  DEFAULT 500,
    CONSTRAINT pk_customer PRIMARY KEY (id),
    UNIQUE KEY uq_customer_email (email)
);

CREATE TABLE order_header (
    id             INT             AUTO_INCREMENT,
    customer_id    INT             NOT NULL REFERENCES customer(id) ON DELETE CASCADE ON UPDATE CASCADE,
    warehouse_id   INT             REFERENCES warehouse(id) ON DELETE RESTRICT ON UPDATE CASCADE,
    order_status   ENUM('pending','paid','shipped','delivered','cancelled','refunded') DEFAULT 'pending',
    placed_at      DATETIME        NOT NULL,
    shipped_at     DATETIME,
    subtotal       DECIMAL(12,2)   DEFAULT 0,
    discount_pct   TINYINT         DEFAULT 0 CHECK (discount_pct BETWEEN 0 AND 100),
    shipping_fee   DECIMAL(8,2)    DEFAULT 0,
    total          DECIMAL(12,2)   DEFAULT 0,
    notes          TEXT,
    is_gift        BOOLEAN         DEFAULT FALSE,
    CONSTRAINT pk_order_header PRIMARY KEY (id)
);

create table order_item(id INT AUTO_INCREMENT, order_id INT NOT NULL REFERENCES order_header(id) ON DELETE CASCADE ON UPDATE CASCADE, product_id INT NOT NULL REFERENCES product(id) ON DELETE RESTRICT ON UPDATE CASCADE, quantity SMALLINT NOT NULL CHECK (quantity > 0), unit_price DECIMAL(10,2) NOT NULL, line_total DECIMAL(12,2), constraint pk_order_item primary key (id));

CREATE TABLE review (
    id                     INT       AUTO_INCREMENT,
    product_id             INT       NOT NULL REFERENCES product(id) ON DELETE CASCADE ON UPDATE CASCADE,
    customer_id            INT       NOT NULL REFERENCES customer(id) ON DELETE CASCADE ON UPDATE CASCADE,
    rating                 TINYINT   CHECK (rating BETWEEN 1 AND 5),
    title                  VARCHAR(150),
    review_text                   TEXT,
    is_verified_purchase   BOOLEAN   DEFAULT FALSE,
    posted_at              TIMESTAMP,
    CONSTRAINT pk_review PRIMARY KEY (id)
);

/* ============================================================ */
/* SECTION 2 : indexes -- btree + hash                            */
/* ============================================================ */
CREATE INDEX idx_category_parent ON category (parent_id);
CREATE INDEX idx_product_category ON product (category_id);
CREATE INDEX idx_product_supplier ON product (supplier_id);
CREATE INDEX idx_product_status ON product (status);
CREATE INDEX idx_product_cat_price ON product (category_id, unit_price);
CREATE INDEX idx_order_customer ON order_header (customer_id);
CREATE INDEX idx_order_status ON order_header (order_status);
CREATE INDEX idx_order_item_order ON order_item (order_id);
CREATE INDEX idx_review_product ON review (product_id);
CREATE INDEX idx_product_sku ON product (sku) USING HASH;
CREATE INDEX idx_customer_email ON customer (email) USING HASH;

/* ============================================================ */
/* SECTION 3 : views                                              */
/* ============================================================ */
CREATE VIEW v_active_products AS SELECT id, sku, name, unit_price, stock_qty, category_id FROM product WHERE status = 'active';
CREATE VIEW v_vip_customers   AS SELECT id, full_name, email, tier, loyalty_points FROM customer WHERE is_vip = true;
CREATE VIEW v_low_stock       AS SELECT id, sku, name, stock_qty, reorder_level FROM product WHERE stock_qty <= reorder_level;
CREATE VIEW v_order_summary   AS SELECT id, customer_id, order_status, total FROM order_header;
CREATE VIEW v_top_suppliers   AS SELECT id, name, country, rating FROM supplier WHERE rating >= 4.0;

/* ============================================================ */
/* SECTION 4 : verify DDL                                         */
/* ============================================================ */
SHOW TABLES;
DESCRIBE product;
SHOW INDEX FROM order_item;
SHOW CREATE TABLE customer;
SHOW CREATE VIEW v_active_products;
CREATE DATABASE IF NOT EXISTS boutique;
CREATE TABLE IF NOT EXISTS category (dummy INT);

/* ============================================================ */
/* SECTION 5 : seed data -- category (3 levels, for recursive CTE) */
/* ============================================================ */
INSERT INTO category (parent_id, name, slug, is_active, display_order, created_at) VALUES
    (NULL, 'Electronics',    'electronics',   true, 1, '2020-01-01 00:00:00'),
    (NULL, 'Fashion',        'fashion',       true, 2, '2020-01-05 00:00:00'),
    (NULL, 'Home & Garden',  'home-garden',   true, 3, '2020-01-10 00:00:00');
INSERT INTO category (parent_id, name, slug, is_active, display_order, created_at) VALUES
    (1, 'Laptops',           'laptops',       true, 1, '2020-02-01 00:00:00'),
    (1, 'Smartphones',       'smartphones',   true, 2, '2020-02-05 00:00:00'),
    (2, 'Mens Clothing',     'mens-clothing', true, 1, '2020-02-10 00:00:00'),
    (2, 'Womens Clothing',   'womens-clothing', true, 2, '2020-02-15 00:00:00'),
    (3, 'Kitchen',           'kitchen',       true, 1, '2020-02-20 00:00:00');
INSERT INTO category (parent_id, name, slug, is_active, display_order, created_at) VALUES
    (4, 'Ultrabooks', 'ultrabooks', true, 1, '2020-03-01 00:00:00');

/* supplier: senior-engineer style, heavily aligned */
INSERT INTO supplier (name, country, rating, certifications, contact, onboarded, contract_expiry, is_preferred) VALUES
    ('Nordic Components',  'Sweden', 4.7, 'iso9001,organic',        '{"email":"sales@nordic.example","phone":"+46-8-555-0101"}', '2018-04-01','2027-04-01', true ),
    ('Pacific Textiles',   'Vietnam',4.2, 'fairtrade',               '{"email":"info@pacifictex.example"}',                       '2019-08-15','2026-08-15', true ),
    ('Everest Hardware',   'China',  3.8, 'iso9001',                 '{"email":"contact@everesthw.example"}',                     '2017-01-10','2025-01-10', false),
    ('GreenLeaf Organics', 'USA',    4.9, 'organic,cruelty_free',    '{"email":"hello@greenleaf.example"}',                       '2021-06-01','2028-06-01', true ),
    ('Budget Imports Co',  'India',  2.9, NULL,                      '{"email":"biz@budgetimports.example"}',                     '2016-03-20','2024-03-20', false);

/* product: junior dev style, compact one-row-per-line, minimal spacing */
INSERT INTO product (sku,name,category_id,supplier_id,unit_price,cost_price,weight_kg,stock_qty,reorder_level,status,tags,attributes,thumbnail,launched_at,last_restocked,is_taxable) VALUES
('LAP-001','ProBook Ultra 14',9,1,1899.99,1200.00,1.3,25,5,'active','featured,new','{"cpu":"i7","ram_gb":16}','thumb-lap001',      '2023-01-15','2026-06-01 10:00:00',true),
('LAP-002','ValueBook 15',    4,3, 699.50, 480.00,1.8,60,10,'active','sale',       '{"cpu":"i5","ram_gb":8}', 'thumb-lap002',      '2022-05-10','2026-06-02 09:30:00',true),
('PHN-001','GalaxyMax X',     5,1,1099.00, 700.00,0.19,80,15,'active','new,featured','{"storage_gb":256}',    'thumb-phn001',      '2023-09-01','2026-06-03 11:00:00',true),
('PHN-002','BudgetPhone Lite',5,5, 199.99, 120.00,0.17,150,20,'active','sale,clearance','{"storage_gb":64}',  'thumb-phn002',      '2021-02-20','2026-06-01 08:00:00',true),
('MEN-001','Classic Oxford Shirt',6,2, 49.99, 18.00,0.3,200,30,'active','new',     '{"material":"cotton"}',  'thumb-men001',      '2022-11-01','2026-05-20 14:00:00',true),
('WMN-001','Silk Blouse',       7,2, 79.99, 30.00,0.2,90,15,'active',NULL,         '{"material":"silk"}',    'thumb-wmn001',      '2023-03-05','2026-05-22 14:30:00',true),
('KIT-001','Chef Knife Set',    8,3, 129.99, 55.00,1.1,40,8,'active','featured',   '{"pieces":6}',           'thumb-kit001',      '2022-07-01','2026-05-25 09:00:00',true),
('KIT-002','Organic Cutting Board',8,4, 39.99, 15.00,0.9,70,10,'active','new', '{"wood":"bamboo"}',      'thumb-kit002',      '2023-01-20','2026-05-28 09:15:00',true),
('LAP-003','Ultrabook Air 13', 9,1,2199.00,1500.00,1.0,12,5,'active','featured,new,limited','{"cpu":"i9","ram_gb":32}','thumb-lap003','2024-02-01','2026-06-04 10:30:00',true),
('PHN-003','RetroFlip',        5,5, 89.99, 45.00,0.15,5,10,'backordered','clearance','{"storage_gb":16}',    'thumb-phn003',      '2019-05-01', NULL,                true),
('KIT-003','Discontinued Blender',8,3, 59.99, 25.00,2.5,0,5,'discontinued',NULL,   '{"watts":500}',          'thumb-kit004',      '2018-01-01', NULL,                true),
('WMN-002','Cashmere Scarf',    7,4,149.00, 60.00,0.1,30,5,'draft','new',          '{"material":"cashmere"}','thumb-wmn002',      NULL,          NULL,                true);

INSERT INTO warehouse (code, city, capacity_units, opened_year, opening_time, manager_name, is_operational) VALUES
    ('NYC1', 'New York',    500000, 2015, '06:00:00', 'Diane Foster',  true),
    ('LAX1', 'Los Angeles', 750000, 2018, '05:30:00', 'Marcus Webb',   true),
    ('ORD1', 'Chicago',     300000, 2020, '07:00:00', 'Priya Nair',    false);

INSERT INTO inventory (product_id, warehouse_id, quantity, last_counted) VALUES
    (1,1,15,'2026-06-01 12:00:00'),(1,2,10,'2026-06-01 12:00:00'),
    (2,1,40,'2026-06-01 12:00:00'),(2,2,20,'2026-06-01 12:00:00'),
    (3,1,50,'2026-06-01 12:00:00'),(3,2,30,'2026-06-01 12:00:00'),
    (4,2,150,'2026-06-01 12:00:00'),
    (5,1,120,'2026-06-01 12:00:00'),(5,3,80,'2026-06-01 12:00:00'),
    (6,1,60,'2026-06-01 12:00:00'),(6,2,30,'2026-06-01 12:00:00'),
    (7,3,40,'2026-06-01 12:00:00'),
    (8,3,70,'2026-06-01 12:00:00'),
    (9,1,12,'2026-06-01 12:00:00'),
    (10,2,5,'2026-06-01 12:00:00'),
    (11,3,0,'2026-06-01 12:00:00');

/* customer: referral chain baked in for recursive-CTE tests. Self-referential FK
   rows must land in earlier, already-committed statements -- a same-batch forward
   reference to a sibling row in the same INSERT is not visible to the FK check yet --
   so the referral tree is seeded one depth-level per statement, ids 1..10 in order. */
INSERT INTO customer (full_name, email, phone, birth_date, signup_at, tier, interests, loyalty_points, preferences, is_vip, credit_limit) VALUES
    ('Olivia Martinez','olivia.m@example.com','555-0100','1988-04-12','2019-01-05 10:00:00','platinum','electronics,home',      4200,'{"newsletter":true}', true, 5000.00),
    ('Ethan Clark',    'ethan.c@example.com', '555-0101','1992-07-30','2019-03-11 09:15:00','gold',    'fashion,sports',          2100,'{"newsletter":false}',false,2000.00),
    ('Isabella Wright','isabella.w@example.com','555-0108','1987-02-27','2018-11-11 11:11:00','platinum','electronics,fashion,home',5000,'{"newsletter":true}', true, 5000.00);

INSERT INTO customer (referred_by, full_name, email, phone, birth_date, signup_at, tier, interests, loyalty_points, preferences, is_vip, credit_limit) VALUES
    (1,'Ava Rodriguez',  'ava.r@example.com',   '555-0102','1995-11-02','2020-02-14 14:20:00','gold',    'electronics,books',       1800,'{"newsletter":true}', false,2000.00),
    (1,'Noah Lewis',     'noah.l@example.com',  '555-0103','1990-05-18','2020-04-22 11:00:00','silver',  'home,beauty',              900,'{"newsletter":true}', false,1000.00),
    (2,'Liam Hall',      'liam.h@example.com',  '555-0105','1985-12-25','2019-06-01 08:30:00','bronze',  'sports,books',             300,'{"newsletter":false}',false, 500.00);

INSERT INTO customer (referred_by, full_name, email, phone, birth_date, signup_at, tier, interests, loyalty_points, preferences, is_vip, credit_limit) VALUES
    (4,'Mia Walker',     'mia.w@example.com',   '555-0104','1998-09-09','2021-01-30 16:45:00','silver',  'fashion',                  650,'{"newsletter":false}',false,1000.00),
    (5,'Sophia Young',   'sophia.y@example.com','555-0106','2000-03-14','2022-05-17 13:00:00','bronze',  'beauty,fashion',           120,'{"newsletter":true}', false, 500.00),
    (6,'Benjamin Scott', 'benjamin.s@example.com','555-0109','1991-10-19','2020-08-08 08:08:00','gold',  'electronics',              1500,'{"newsletter":true}', false,2000.00);

INSERT INTO customer (referred_by, full_name, email, phone, birth_date, signup_at, tier, interests, loyalty_points, preferences, is_vip, credit_limit) VALUES
    (7,'James King',     'james.k@example.com', '555-0107','1993-08-08','2022-09-09 09:09:00','bronze',  NULL,                        50,'{"newsletter":false}',false, 500.00);

/* order_header + order_item: written as one dense block by an impatient author */
INSERT INTO order_header (customer_id,warehouse_id,order_status,placed_at,shipped_at,subtotal,discount_pct,shipping_fee,total,notes,is_gift) VALUES
(1,1,'delivered','2026-01-05 10:00:00','2026-01-06 09:00:00',1899.99,10,0.00,1709.99,'Priority customer',false),
(1,2,'delivered','2026-02-10 11:30:00','2026-02-11 09:00:00', 129.99, 0,9.99, 139.98,NULL,false),
(2,1,'shipped',  '2026-03-01 09:15:00','2026-03-02 10:00:00', 249.98, 5,9.99, 246.47,NULL,true),
(3,2,'paid',     '2026-03-15 14:00:00',NULL,                 1099.00, 0,0.00,1099.00,'Gift wrap requested',true),
(4,1,'pending',  '2026-04-01 08:00:00',NULL,                   79.99, 0,4.99,  84.98,NULL,false),
(6,3,'cancelled','2026-04-10 16:20:00',NULL,                  199.99, 0,0.00, 199.99,'Customer requested cancel',false),
(9,1,'delivered','2026-01-20 12:00:00','2026-01-21 09:00:00',2199.00,15,0.00,1869.15,'VIP unboxing',false),
(9,2,'delivered','2026-05-01 10:10:00','2026-05-02 09:00:00', 169.98, 0,0.00, 169.98,NULL,false);

INSERT INTO order_item (order_id,product_id,quantity,unit_price,line_total) VALUES
(1,1,1,1899.99,1899.99),
(2,7,1, 129.99, 129.99),
(3,4,1, 199.99, 199.99),
(3,10,1, 89.99, 89.99),
(4,3,1,1099.00,1099.00),
(5,6,1,  79.99,  79.99),
(6,4,1, 199.99, 199.99),
(7,9,1,2199.00,2199.00),
(8,7,1, 129.99, 129.99),(8,8,1,39.99,39.99);

/* review: mix of short & long free-text bodies */
INSERT INTO review (product_id, customer_id, rating, title, review_text, is_verified_purchase, posted_at) VALUES
    (1,1,5,'Excellent build quality','Fast, quiet, and the battery life is outstanding for daily work.',true,'2026-01-10 12:00:00'),
    (7,1,4,'Great knives','Sharp out of the box, handles feel solid.',true,'2026-02-15 09:00:00'),
    (4,2,3,'Decent phone','Camera is fine but battery drains faster than expected.',true,'2026-03-10 10:00:00'),
    (3,3,5,'Loved it',   'Screen is gorgeous, exactly as advertised.',true,'2026-03-20 15:00:00'),
    (6,4,2,'Not what I expected','Fabric feels thinner than the photos suggested.',true,'2026-04-05 11:00:00'),
    (1,5,4,NULL,'Solid laptop for the price range.',false,'2026-04-12 08:30:00'),
    (9,9,5,'Best laptop I have owned','Worth every cent, upgraded from an old ultrabook.',true,'2026-01-25 13:00:00'),
    (7,9,5,'Perfect gift','Bought as a gift and the recipient loved it.',true,'2026-05-05 09:45:00'),
    (2,6,1,'Arrived damaged','Screen was cracked on arrival, requested a refund.',false,'2026-04-15 17:00:00'),
    (10,8,3,'Cute retro design','Battery is weak but the design is charming.',false,'2026-02-01 10:15:00');

/* ============================================================ */
/* SECTION 6 : basic SELECT -- filters, predicates, JSON access   */
/* ============================================================ */
SELECT id, name, unit_price, stock_qty FROM product WHERE unit_price >= 500 AND status = 'active' ORDER BY unit_price DESC;
SELECT name, sku FROM product ORDER BY launched_at LIMIT 1, 3;
SELECT DISTINCT status FROM product ORDER BY status;
SELECT name FROM product WHERE category_id IN (4,5) AND unit_price BETWEEN 100 AND 2000;
SELECT name, sku FROM product WHERE name LIKE '%Book%' OR category_id IS NULL;
SELECT name FROM product WHERE name REGEXP '^[A-C]';
SELECT name FROM product WHERE name NOT REGEXP '^[A-C]' ORDER BY name LIMIT 5;
select name, REGEXP_LIKE(name,'^L') as starts_l, REGEXP_REPLACE(sku,'-[0-9]+$','') as sku_family from product limit 3;
SELECT name, unit_price FROM product WHERE NOT (unit_price < 100);
SELECT id, attributes->>'$.cpu' AS cpu, attributes->>'$.ram_gb' AS ram FROM product WHERE category_id IN (4,9) ORDER BY id;
SELECT id, preferences->>'$.newsletter' AS newsletter FROM customer LIMIT 4;
SELECT id, JSON_EXTRACT(contact,'$.email') AS email, JSON_VALUE(contact,'$.phone') AS phone FROM supplier ORDER BY id LIMIT 3;
SELECT id, JSON_UNQUOTE(JSON_EXTRACT(attributes,'$.cpu')) AS cpu_unquoted FROM product WHERE attributes->>'$.cpu' IS NOT NULL ORDER BY id LIMIT 3;

/* ============================================================ */
/* SECTION 7 : aggregates, incl. aggregate-CASE idioms            */
/* ============================================================ */
SELECT COUNT(*), SUM(unit_price), AVG(unit_price), MAX(unit_price), MIN(unit_price) FROM product;
SELECT category_id, COUNT(*) AS n_products, AVG(unit_price) AS avg_price FROM product GROUP BY category_id HAVING n_products >= 2 ORDER BY avg_price DESC;
SELECT status, GROUP_CONCAT(sku SEPARATOR ', ') AS skus FROM product GROUP BY status ORDER BY status;
SELECT COUNT(DISTINCT category_id), SUM(DISTINCT reorder_level), STDDEV(unit_price), VARIANCE(unit_price) FROM product;
SELECT
    warehouse_id,
    SUM(CASE WHEN order_status IN ('paid','shipped','delivered') THEN total ELSE 0 END) AS recognized_revenue,
    SUM(CASE WHEN order_status = 'cancelled' THEN 1 ELSE 0 END) AS cancelled_count,
    COUNT(CASE WHEN is_gift = true THEN 1 END) AS gift_orders
FROM order_header
GROUP BY warehouse_id
ORDER BY recognized_revenue DESC;
SELECT category_id, SUM(warehouse_id IS NULL) AS no_warehouse_flagged, COUNT(supplier_id IS NOT NULL) AS has_supplier FROM product GROUP BY category_id ORDER BY category_id;

/* ============================================================ */
/* SECTION 8 : parser-compatibility grab bag                      */
/* ============================================================ */
SELECT id, name, status FROM product WHERE status <> 'discontinued' AND category_id <> 8 ORDER BY id LIMIT 4;
SELECT name || ' (' || sku || ')' AS label FROM product ORDER BY id LIMIT 4;
SELECT name, stock_qty, stock_qty % 10 AS remainder_10 FROM product ORDER BY id LIMIT 4;
SELECT id, name, unit_price FROM product ORDER BY unit_price DESC LIMIT 2 OFFSET 2;
SELECT name, is_taxable FROM product WHERE is_taxable = TRUE ORDER BY id LIMIT 4;
SELECT name, is_taxable FROM product WHERE is_taxable = FALSE ORDER BY id;
SELECT `name`, `unit_price` FROM `product` WHERE `category_id` = 9 ORDER BY `unit_price` DESC;

/* ============================================================ */
/* SECTION 9 : joins -- inner/left/right/full/cross/natural/using */
/* ============================================================ */
SELECT p.name, c.name AS category_name, p.unit_price FROM product p JOIN category c ON p.category_id = c.id ORDER BY p.unit_price DESC;
SELECT p.name, c.name AS category, s.name AS supplier FROM product p JOIN category c ON p.category_id=c.id JOIN supplier s ON p.supplier_id=s.id ORDER BY p.name;
SELECT p.name, c.name AS category_name FROM product p LEFT JOIN category c ON p.category_id = c.id ORDER BY p.id;
SELECT c.name AS category_name, p.name AS product_name FROM category c RIGHT JOIN product p ON c.id = p.category_id ORDER BY p.id;
SELECT c.name, p.name AS product_name FROM category c FULL OUTER JOIN product p ON c.id = p.category_id ORDER BY c.name LIMIT 10;
SELECT c.name AS category, s.name AS supplier FROM category c CROSS JOIN supplier s ORDER BY c.name, s.name LIMIT 6;

-- self join: customer referral pairs
SELECT cust.full_name AS customer, ref.full_name AS referred_by_name FROM customer cust JOIN customer ref ON cust.referred_by = ref.id ORDER BY cust.id;

/* 4-table reporting join, formatted verbosely on purpose */
SELECT
    cu.full_name                              AS customer_name,
    oh.id                                      AS order_id,
    oh.order_status                            AS status,
    pr.name                                    AS product_name,
    oi.quantity                                AS qty,
    oi.line_total                              AS line_total
FROM order_header oh
JOIN customer cu   ON oh.customer_id = cu.id
JOIN order_item oi ON oi.order_id    = oh.id
JOIN product pr    ON oi.product_id  = pr.id
WHERE oh.order_status IN ('paid','shipped','delivered')
ORDER BY oh.id, pr.name;

/* ============================================================ */
/* SECTION 10 : subqueries -- correlated, nested, EXISTS, derived  */
/* ============================================================ */
SELECT name, unit_price FROM product WHERE unit_price > (SELECT AVG(unit_price) FROM product);
SELECT full_name, email FROM customer WHERE id IN (SELECT customer_id FROM order_header WHERE total > 1000);
SELECT name FROM product WHERE EXISTS (SELECT 1 FROM review r WHERE r.product_id = product.id AND r.rating >= 5);
SELECT name FROM product WHERE NOT EXISTS (SELECT 1 FROM order_item oi WHERE oi.product_id = product.id);

-- correlated subquery: products priced above their own category's average
SELECT p1.name, p1.unit_price, p1.category_id
FROM product p1
WHERE p1.unit_price > (SELECT AVG(p2.unit_price) FROM product p2 WHERE p2.category_id = p1.category_id)
ORDER BY p1.category_id, p1.unit_price DESC;

-- nested subquery (two levels): customers who ordered a product from a supplier
-- whose rating beats the average rating of every supplier in 'China'
SELECT DISTINCT cu.full_name
FROM customer cu
JOIN order_header oh ON oh.customer_id = cu.id
JOIN order_item oi   ON oi.order_id = oh.id
JOIN product pr      ON oi.product_id = pr.id
WHERE pr.supplier_id IN (
    SELECT s.id FROM supplier s
    WHERE s.rating > (SELECT AVG(rating) FROM supplier WHERE country = 'China')
)
ORDER BY cu.full_name;

SELECT status, avg_total FROM (SELECT order_status AS status, AVG(total) AS avg_total FROM order_header GROUP BY order_status) AS sub WHERE avg_total > 100;
SELECT full_name, (SELECT MAX(total) FROM order_header) AS biggest_order_overall FROM customer ORDER BY loyalty_points DESC LIMIT 3;
SELECT name FROM product WHERE category_id NOT IN (SELECT id FROM category WHERE is_active = false);

/* ============================================================ */
/* SECTION 11 : set operations                                    */
/* ============================================================ */
SELECT full_name AS label, 'customer' AS entity FROM customer WHERE tier = 'platinum'
UNION
SELECT name, 'supplier' AS entity FROM supplier WHERE is_preferred = true
ORDER BY label;

SELECT customer_id AS who FROM order_header WHERE order_status = 'delivered'
UNION ALL
SELECT customer_id FROM order_header WHERE total >= 500
ORDER BY who;

SELECT customer_id FROM order_header WHERE total >= 100
INTERSECT
SELECT customer_id FROM order_header WHERE order_status = 'delivered';

SELECT id FROM customer
EXCEPT
SELECT customer_id FROM order_header WHERE customer_id IS NOT NULL;

/* ============================================================ */
/* SECTION 12 : CTEs -- plain, chained, and recursive             */
/* ============================================================ */
WITH big_spender AS (
    SELECT customer_id, SUM(total) AS lifetime_value FROM order_header GROUP BY customer_id
)
SELECT cu.full_name, bs.lifetime_value, cu.tier
FROM big_spender bs JOIN customer cu ON bs.customer_id = cu.id
ORDER BY bs.lifetime_value DESC;

WITH cat_counts AS (
    SELECT category_id, COUNT(*) AS n_products FROM product GROUP BY category_id
),
order_counts AS (
    SELECT pr.category_id, COUNT(*) AS n_orders
    FROM order_item oi JOIN product pr ON oi.product_id = pr.id
    GROUP BY pr.category_id
)
SELECT c.name, cc.n_products, oc.n_orders
FROM category c
LEFT JOIN cat_counts cc   ON c.id = cc.category_id
LEFT JOIN order_counts oc ON c.id = oc.category_id
ORDER BY c.id;

-- recursive CTE #1: category tree with depth and a breadcrumb path
WITH RECURSIVE cat_tree AS (
    SELECT id, name, parent_id, 0 AS depth, name AS path FROM category WHERE parent_id IS NULL
    UNION ALL
    SELECT c.id, c.name, c.parent_id, t.depth + 1, CONCAT(t.path, ' > ', c.name)
    FROM category c JOIN cat_tree t ON c.parent_id = t.id
)
SELECT id, name, depth, path FROM cat_tree ORDER BY path;

-- recursive CTE #2: customer referral chain
WITH RECURSIVE referral_chain AS (
    SELECT id, full_name, referred_by, 0 AS depth
    FROM customer WHERE referred_by IS NULL
    UNION ALL
    SELECT cu.id, cu.full_name, cu.referred_by, rc.depth + 1
    FROM customer cu JOIN referral_chain rc ON cu.referred_by = rc.id
)
SELECT id, full_name, depth FROM referral_chain ORDER BY depth, id;

/* ============================================================ */
/* SECTION 13 : window functions                                  */
/* ============================================================ */
SELECT name, unit_price,
    ROW_NUMBER()   OVER (ORDER BY unit_price DESC)                              AS overall_rank,
    RANK()         OVER (PARTITION BY category_id ORDER BY unit_price DESC)     AS cat_rank,
    DENSE_RANK()   OVER (PARTITION BY category_id ORDER BY unit_price DESC)     AS cat_dense,
    LAG(unit_price,1)  OVER (PARTITION BY category_id ORDER BY unit_price)      AS prev_price,
    LEAD(unit_price,1) OVER (PARTITION BY category_id ORDER BY unit_price)      AS next_price,
    FIRST_VALUE(unit_price) OVER (PARTITION BY category_id ORDER BY unit_price DESC) AS top_in_cat
FROM product WHERE category_id IS NOT NULL ORDER BY category_id, unit_price DESC;

SELECT id, unit_price,
    SUM(unit_price) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW)  AS running_total,
    AVG(unit_price) OVER (ORDER BY id ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING)          AS moving_avg,
    NTH_VALUE(unit_price,2) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS second_price,
    NTILE(3)       OVER (ORDER BY unit_price) AS price_bucket,
    PERCENT_RANK() OVER (ORDER BY unit_price) AS pct_rank,
    CUME_DIST()    OVER (ORDER BY unit_price) AS cume_d
FROM product ORDER BY id;

SELECT category_id,
    SUM(unit_price)  OVER (PARTITION BY category_id) AS cat_price_total,
    COUNT(*)         OVER (PARTITION BY category_id) AS cat_product_count,
    MAX(stock_qty)   OVER (PARTITION BY category_id) AS max_stock_in_cat,
    SUM(unit_price)  OVER (ORDER BY unit_price RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS price_range_running
FROM product ORDER BY category_id, unit_price;

/* window function built on top of an aggregate CASE expression */
SELECT warehouse_id, order_status,
    SUM(CASE WHEN is_gift = true THEN 1 ELSE 0 END) OVER (PARTITION BY warehouse_id ORDER BY id) AS gift_orders_running
FROM order_header ORDER BY warehouse_id, id;

/* ============================================================ */
/* SECTION 14 : scalar functions -- string / math / date / json   */
/* ============================================================ */
SELECT UPPER(name), LOWER(sku), LENGTH(name), CONCAT(sku,' - ',name) AS label FROM product LIMIT 3;
SELECT TRIM('  padded  ') AS trimmed, LTRIM('  left') AS l_trim, RTRIM('right  ') AS r_trim, SPACE(3) AS three_spaces, LENGTH(SPACE(3)) AS three_spaces_len;
SELECT SUBSTR(sku,1,3) AS prefix, SUBSTRING(name,1,5) AS name_prefix, REPLACE(sku,'-','_') AS underscored, LPAD(id,5,'0') AS padded_id FROM product LIMIT 3;
SELECT LEFT(name,4) AS pfx, RIGHT(name,4) AS sfx, REVERSE(sku) AS rev, REPEAT('*',3) AS stars, INSTR(name,'o') AS o_pos, LOCATE('o',name) AS o_pos2, ASCII('Z') AS ascii_z FROM product LIMIT 3;
SELECT HEX(255) AS hex_255, UNHEX('4D7953514C') AS unhex_val, FORMAT(unit_price,2) AS fmt_price FROM product LIMIT 3;
SELECT ROUND(3.14159,2), ABS(-42.5), CEIL(4.1), FLOOR(4.9), MOD(29,6), SQRT(256), POW(3,4), LOG2(64), LOG10(10000), LOG(8,2) AS log_base2_of_8, PI(), SIGN(-7), TRUNCATE(5.6789,2), RAND() BETWEEN 0 AND 1 AS rand_in_range, EXP(1) AS e_to_1, SIN(0) AS sin_0, COS(0) AS cos_0, TAN(0) AS tan_0;
SELECT YEAR(launched_at), MONTH(launched_at), DAY(launched_at), DAYOFMONTH(launched_at), DAYOFWEEK(launched_at), DAYOFYEAR(launched_at), WEEKDAY(launched_at), LAST_DAY(launched_at) FROM product WHERE launched_at IS NOT NULL LIMIT 3;
SELECT DATEDIFF('2026-06-01',launched_at) AS days_on_market, DATE_FORMAT(launched_at,'%Y-%m') AS launch_month, DATE_ADD(launched_at, INTERVAL 1 YEAR) AS one_yr_anniversary, DATE_SUB(launched_at, INTERVAL 1 MONTH) AS one_month_before, TIMESTAMPDIFF(DAY,launched_at,'2026-06-01') AS days_diff, TIMESTAMPDIFF(WEEK,launched_at,'2026-06-01') AS weeks_diff FROM product WHERE launched_at IS NOT NULL LIMIT 3;
SELECT COALESCE(category_id,-1) AS cat_or_default, IFNULL(category_id,-1) AS cat_ifnull, NULLIF(status,'draft') AS nullif_draft, GREATEST(1,9,4), LEAST(1,9,4), CAST(stock_qty AS INT) AS stock_int, IF(unit_price>500,'Premium','Standard') AS price_tier, MD5(sku), LENGTH(UUID()) > 0 AS uuid_ok, ISNULL(category_id) AS cat_is_null, BIT_LENGTH(sku) AS sku_bits FROM product LIMIT 3;
SELECT name,
    CASE WHEN unit_price >= 1500 THEN 'Flagship' WHEN unit_price >= 500 THEN 'Mid-range' ELSE 'Budget' END AS price_segment
FROM product ORDER BY unit_price DESC LIMIT 6;
SELECT CONCAT_WS(' | ', sku, name, status) AS summary FROM product LIMIT 4;
SELECT CAST(unit_price AS SIGNED) AS price_int, CAST(rating AS DECIMAL) AS rating_dec, CAST(onboarded AS DATE) AS onboarded_d FROM supplier LIMIT 3;
SELECT CONVERT(unit_price, CHAR) AS price_str, CONVERT(stock_qty, UNSIGNED) AS stock_uint, CAST('2025-12-25' AS DATE) AS parsed_holiday FROM product LIMIT 3;
SELECT NOW() IS NOT NULL AS now_ok, CURDATE() IS NOT NULL AS curdate_ok, CURTIME() IS NOT NULL AS curtime_ok, CURRENT_TIMESTAMP IS NOT NULL AS curts_ok, UNIX_TIMESTAMP() >= 0 AS unixts_ok, VERSION() IS NOT NULL AS version_ok, DATABASE() AS current_db, CURRENT_USER() IS NOT NULL AS user_ok;

/* ============================================================ */
/* SECTION 15 : insert variants, ON DUPLICATE KEY, INSERT..SELECT */
/* ============================================================ */
INSERT IGNORE INTO supplier (name, country, rating) VALUES ('Nordic Components', 'Duplicate Attempt', 0);

INSERT INTO product (sku,name,category_id,supplier_id,unit_price,status)
    VALUES ('LAP-001','ProBook Ultra 14 (dup)',9,1,1899.99,'active')
    ON DUPLICATE KEY UPDATE unit_price = unit_price - 100;
SELECT sku, name, unit_price FROM product WHERE sku = 'LAP-001';
UPDATE product SET unit_price = 1899.99 WHERE sku = 'LAP-001';

CREATE TABLE product_archive (id INT PRIMARY KEY, sku VARCHAR(30), name VARCHAR(150), unit_price DECIMAL(10,2));
INSERT INTO product_archive SELECT id, sku, name, unit_price FROM product WHERE status = 'discontinued';
SELECT * FROM product_archive ORDER BY id;
TRUNCATE TABLE product_archive;
DROP TABLE product_archive;

/* ============================================================ */
/* SECTION 16 : RETURNING                                         */
/* ============================================================ */
INSERT INTO category (name, slug, is_active) VALUES ('Temp Category','temp-category',true) RETURNING id, name, slug;
DELETE FROM category WHERE slug = 'temp-category' RETURNING id, name;
UPDATE product SET stock_qty = stock_qty + 10 WHERE id = 2 RETURNING id, name, stock_qty;
UPDATE product SET stock_qty = stock_qty - 10 WHERE id = 2;

/* ============================================================ */
/* SECTION 17 : multi-table UPDATE / DELETE                       */
/* ============================================================ */
UPDATE product p, category c SET p.stock_qty = p.stock_qty + 5, c.display_order = c.display_order + 1
    WHERE p.category_id = c.id AND c.slug = 'laptops';
UPDATE product SET stock_qty = stock_qty - 5 WHERE category_id = 4;
DELETE order_item FROM order_item
    JOIN order_header ON order_item.order_id = order_header.id
    WHERE order_header.order_status = 'cancelled';

/* ============================================================ */
/* SECTION 18 : ALTER TABLE                                       */
/* ============================================================ */
ALTER TABLE product ADD COLUMN warranty_months INT;
UPDATE product SET warranty_months = 24 WHERE category_id IN (4,5,9);
ALTER TABLE product RENAME COLUMN warranty_months TO warranty_period_months;
SELECT id, name, warranty_period_months FROM product WHERE warranty_period_months IS NOT NULL ORDER BY id;
ALTER TABLE product DROP COLUMN warranty_period_months;
ALTER TABLE product MODIFY COLUMN status ENUM('draft','active','discontinued','backordered') DEFAULT 'active';
ALTER TABLE supplier ADD CONSTRAINT uq_supplier_name UNIQUE (name);
ALTER TABLE supplier DROP CONSTRAINT uq_supplier_name;
ALTER TABLE order_item ADD CONSTRAINT chk_qty_positive CHECK (quantity >= 1);
ALTER TABLE order_item DROP CONSTRAINT chk_qty_positive;

/* ============================================================ */
/* SECTION 19 : ENUM / SET edge cases                             */
/* ============================================================ */
INSERT INTO product (sku,name,category_id,supplier_id,unit_price,status) VALUES ('BAD-001','Bad Status Product',1,1,10.00,'vaporware');
INSERT INTO customer (full_name,email,tier) VALUES ('Bad Tier Person','bad.tier@example.com','diamond');
SELECT sku, status FROM product ORDER BY id DESC LIMIT 3;

/* ============================================================ */
/* SECTION 20 : foreign-key action verification                   */
/* ============================================================ */
DELETE FROM category WHERE slug = 'electronics';

INSERT INTO category (name, slug) VALUES ('ToDelete', 'to-delete');
INSERT INTO product (sku,name,category_id,unit_price,status) SELECT 'DEL-001','Delete Test Product',id,9.99,'active' FROM category WHERE slug = 'to-delete';
SELECT sku, category_id FROM product WHERE sku = 'DEL-001';
DELETE FROM category WHERE slug = 'to-delete';
SELECT sku, category_id FROM product WHERE sku = 'DEL-001';
DELETE FROM product WHERE sku = 'DEL-001';

INSERT INTO customer (full_name,email) VALUES ('Cascade Test','cascade.test@example.com');
INSERT INTO order_header (customer_id, order_status, placed_at, total) SELECT id,'pending','2026-06-01 00:00:00',0 FROM customer WHERE email = 'cascade.test@example.com';
SELECT COUNT(*) AS orders_before FROM order_header WHERE customer_id = (SELECT id FROM customer WHERE email='cascade.test@example.com');
DELETE FROM customer WHERE email = 'cascade.test@example.com';
SELECT COUNT(*) AS orders_after FROM order_header WHERE customer_id = (SELECT id FROM customer WHERE email='cascade.test@example.com');

/* ============================================================ */
/* SECTION 21 : MERGE                                             */
/* ============================================================ */
INSERT INTO supplier (name, country, rating) VALUES ('Temp Supplier', 'Nowhere', 0.0);
CREATE TABLE supplier_upd (name VARCHAR(120) PRIMARY KEY, country VARCHAR(60), rating DOUBLE);
INSERT INTO supplier_upd VALUES
    ('Nordic Components', 'Sweden (updated)', 4.8),
    ('Temp Supplier',      'Nowhere',          0.0),
    ('Fresh Supplier Co',  'Germany',          4.1);
MERGE INTO supplier USING supplier_upd ON supplier.name = supplier_upd.name
    WHEN MATCHED AND supplier_upd.rating = 0.0 THEN DELETE
    WHEN MATCHED THEN UPDATE SET rating = supplier_upd.rating, country = supplier_upd.country
    WHEN NOT MATCHED THEN INSERT (name, country, rating) VALUES (supplier_upd.name, supplier_upd.country, supplier_upd.rating);
SELECT name, country, rating FROM supplier ORDER BY id;
DROP TABLE supplier_upd;

/* ============================================================ */
/* SECTION 22 : stored procedures                                 */
/* ============================================================ */
CREATE PROCEDURE classify_price(IN p_price INT) BEGIN DECLARE tier VARCHAR(20) DEFAULT 'budget'; IF p_price >= 1500 THEN SET tier = 'flagship'; ELSEIF p_price >= 500 THEN SET tier = 'mid-range'; END IF; SELECT tier AS price_tier; END;
CALL classify_price(1899);
CALL classify_price(699);
CALL classify_price(39);
DROP PROCEDURE classify_price;

CREATE PROCEDURE sum_squares(IN n INT) BEGIN DECLARE i INT DEFAULT 1; DECLARE total INT DEFAULT 0; WHILE i <= n DO SET total = total + (i * i); SET i = i + 1; END WHILE; SELECT total AS sum_of_squares; END;
CALL sum_squares(5);
DROP PROCEDURE sum_squares;

CREATE PROCEDURE even_sum(IN n INT) BEGIN DECLARE i INT DEFAULT 0; DECLARE total INT DEFAULT 0; calc: LOOP SET i = i + 1; IF i > n THEN LEAVE calc; END IF; IF MOD(i,2) <> 0 THEN ITERATE calc; END IF; SET total = total + i; END LOOP; SELECT total AS even_sum; END;
CALL even_sum(10);
DROP PROCEDURE even_sum;

CREATE PROCEDURE restock_countdown(IN start_val INT) BEGIN DECLARE counter INT; SET counter = start_val; REPEAT SET counter = counter - 1; UNTIL counter <= 0 END REPEAT; SELECT counter AS final_count; END;
CALL restock_countdown(7);
DROP PROCEDURE restock_countdown;

CREATE PROCEDURE double_stock(IN x INT) BEGIN DECLARE result INT; SET result = x * 2; SELECT result AS doubled_stock; END;
CALL double_stock(30);
DROP PROCEDURE double_stock;

/* ============================================================ */
/* SECTION 23 : user-defined functions                             */
/* ============================================================ */
CREATE FUNCTION apply_discount(price) RETURNS DECIMAL RETURN price * 0.9;
SELECT name, unit_price, apply_discount(unit_price) AS discounted_price FROM product ORDER BY unit_price DESC LIMIT 4;
DROP FUNCTION apply_discount;

CREATE FUNCTION loyalty_label(points) RETURNS TEXT RETURN CONCAT('points=', points);
SELECT full_name, loyalty_points, loyalty_label(loyalty_points) AS label FROM customer ORDER BY loyalty_points DESC LIMIT 5;
DROP FUNCTION loyalty_label;

/* ============================================================ */
/* SECTION 24 : prepared statements, multi-parameter               */
/* ============================================================ */
PREPARE find_by_category_and_price FROM 'SELECT id, name, unit_price FROM product WHERE category_id = ? AND unit_price >= ?';
SET @cat_id = 4;
SET @min_price = 500;
EXECUTE find_by_category_and_price USING @cat_id, @min_price;
SET @cat_id = 8;
SET @min_price = 0;
EXECUTE find_by_category_and_price USING @cat_id, @min_price;
DEALLOCATE PREPARE find_by_category_and_price;
SET @rating_cutoff = 4.5;
SELECT name, rating FROM supplier WHERE rating >= 4.5 ORDER BY rating DESC;

/* ============================================================ */
/* SECTION 25 : triggers -- multi-statement BEGIN..END bodies      */
/* ============================================================ */
CREATE TRIGGER trg_after_insert_order AFTER INSERT ON order_header FOR EACH ROW
BEGIN
    UPDATE customer SET loyalty_points = loyalty_points + 10 WHERE id = 1;
    UPDATE warehouse SET is_operational = true WHERE id = 1;
END;
INSERT INTO order_header (customer_id, order_status, placed_at, total) VALUES (2,'pending','2026-06-05 00:00:00',0);
SELECT loyalty_points FROM customer WHERE id = 1;
DELETE FROM order_header WHERE customer_id = 2 AND total = 0 AND notes IS NULL;
DROP TRIGGER IF EXISTS trg_after_insert_order;

CREATE TRIGGER trg_before_update_product BEFORE UPDATE ON product FOR EACH ROW
    UPDATE warehouse SET is_operational = true WHERE id = 1;
UPDATE product SET stock_qty = stock_qty + 1 WHERE sku = 'KIT-002';
SELECT is_operational FROM warehouse WHERE id = 1;
UPDATE product SET stock_qty = stock_qty - 1 WHERE sku = 'KIT-002';
DROP TRIGGER IF EXISTS trg_before_update_product;

CREATE TRIGGER trg_after_delete_review AFTER DELETE ON review FOR EACH ROW
BEGIN
    UPDATE product SET stock_qty = stock_qty WHERE id = 1;
    UPDATE customer SET loyalty_points = loyalty_points WHERE id = 1;
END;
DELETE FROM review WHERE title = 'Arrived damaged';
DROP TRIGGER IF EXISTS trg_after_delete_review;
INSERT INTO review (product_id, customer_id, rating, title, review_text, is_verified_purchase, posted_at)
    VALUES (2,6,1,'Arrived damaged','Screen was cracked on arrival, requested a refund.',false,'2026-04-15 17:00:00');

/* ============================================================ */
/* SECTION 26 : transactions, savepoints                          */
/* ============================================================ */
BEGIN;
INSERT INTO customer (full_name, email, loyalty_points) VALUES ('Txn Test','txn.test@example.com',0);
SAVEPOINT sp_insert;
UPDATE customer SET loyalty_points = 99999 WHERE email = 'txn.test@example.com';
SELECT loyalty_points FROM customer WHERE email = 'txn.test@example.com';
SAVEPOINT sp_update;
UPDATE customer SET loyalty_points = 1 WHERE email = 'txn.test@example.com';
ROLLBACK TO SAVEPOINT sp_update;
SELECT loyalty_points FROM customer WHERE email = 'txn.test@example.com';
ROLLBACK TO SAVEPOINT sp_insert;
SELECT loyalty_points FROM customer WHERE email = 'txn.test@example.com';
COMMIT;
SELECT full_name, loyalty_points FROM customer WHERE email = 'txn.test@example.com';
DELETE FROM customer WHERE email = 'txn.test@example.com';

BEGIN;
UPDATE supplier SET rating = 0 WHERE name = 'Nordic Components';
ROLLBACK;
SELECT rating FROM supplier WHERE name = 'Nordic Components';

BEGIN;
SAVEPOINT sp_noop;
RELEASE SAVEPOINT sp_noop;
COMMIT;

/* ============================================================ */
/* SECTION 27 : isolation levels                                  */
/* ============================================================ */
SET ISOLATION LEVEL SERIALIZABLE;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL REPEATABLE READ;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ COMMITTED;
SHOW ISOLATION LEVEL;
SET ISOLATION LEVEL READ UNCOMMITTED;
SET ISOLATION LEVEL READ COMMITTED;

/* ============================================================ */
/* SECTION 28 : row locking                                       */
/* ============================================================ */
BEGIN;
SELECT id, name, stock_qty FROM product WHERE id = 1 FOR UPDATE;
SHOW LOCKS;
COMMIT;

BEGIN;
SELECT id, name FROM category WHERE id = 4 FOR SHARE;
SELECT id, name FROM category WHERE id = 5 FOR SHARE;
SHOW LOCKS;
COMMIT;

/* ============================================================ */
/* SECTION 29 : EXPLAIN / access-path selection                   */
/* ============================================================ */
EXPLAIN SELECT * FROM product WHERE id = 1;
EXPLAIN SELECT * FROM product WHERE category_id = 9;
EXPLAIN SELECT * FROM product WHERE sku = 'LAP-001';
SELECT id, name, sku FROM product WHERE sku = 'LAP-001';
EXPLAIN SELECT id, name FROM category WHERE slug = 'laptops';
SELECT id, name FROM category WHERE slug = 'laptops';
EXPLAIN SELECT p.name, c.name, s.name FROM product p
    JOIN category c ON p.category_id = c.id
    JOIN supplier s ON p.supplier_id = s.id;

ANALYZE TABLE category;
ANALYZE TABLE product;
ANALYZE TABLE customer;
CREATE INDEX idx_product_price ON product (unit_price);
EXPLAIN SELECT * FROM product WHERE unit_price > 1000;
EXPLAIN SELECT * FROM product WHERE unit_price < 100;
EXPLAIN SELECT * FROM product WHERE unit_price >= 500;
EXPLAIN SELECT * FROM product WHERE id > 8;
EXPLAIN SELECT * FROM product WHERE id BETWEEN 3 AND 9;
EXPLAIN SELECT * FROM product WHERE category_id > 5;
EXPLAIN ANALYZE SELECT * FROM product WHERE category_id = 9 AND unit_price >= 500;

/* ============================================================ */
/* SECTION 30 : views in action, incl. updatable view              */
/* ============================================================ */
SELECT * FROM v_active_products ORDER BY id;
SELECT * FROM v_vip_customers ORDER BY loyalty_points DESC;
SELECT * FROM v_low_stock ORDER BY stock_qty;
UPDATE v_active_products SET unit_price = 1949.99 WHERE id = 1;
SELECT id, name, unit_price FROM product WHERE id = 1;
UPDATE product SET unit_price = 1899.99 WHERE id = 1;

/* ============================================================ */
/* SECTION 31 : INFORMATION_SCHEMA                                 */
/* ============================================================ */
SELECT table_name, table_rows FROM information_schema.tables WHERE table_schema = 'boutique' ORDER BY table_name;
SELECT column_name, data_type, is_nullable FROM information_schema.columns WHERE table_name = 'product' ORDER BY ordinal_position LIMIT 8;
SELECT constraint_name, table_name, constraint_type FROM information_schema.table_constraints WHERE table_schema = 'boutique' ORDER BY table_name, constraint_name LIMIT 10;

/* ============================================================ */
/* SECTION 32 : FETCH FIRST / FETCH NEXT                            */
/* ============================================================ */
SELECT id, name, unit_price FROM product ORDER BY unit_price DESC FETCH FIRST 3 ROWS ONLY;
SELECT id, name, unit_price FROM product ORDER BY unit_price ASC FETCH NEXT 3 ROWS ONLY;

/* ============================================================ */
/* SECTION 33 : JOIN ... USING / NATURAL JOIN                      */
/* ============================================================ */
CREATE TABLE tmp_region (region_id INT PRIMARY KEY, region_name VARCHAR(50) NOT NULL);
CREATE TABLE tmp_store  (store_name VARCHAR(50) NOT NULL, region_id INT NOT NULL);
INSERT INTO tmp_region VALUES (1,'Northeast'),(2,'West'),(3,'Midwest');
INSERT INTO tmp_store  VALUES ('Downtown',1),('Harbor',2),('Lakeside',3),('Uptown',1);
SELECT region_id, region_name, store_name FROM tmp_region JOIN tmp_store USING (region_id) ORDER BY region_id, store_name;
SELECT region_name, store_name FROM tmp_region NATURAL JOIN tmp_store ORDER BY region_name;
DROP TABLE tmp_store;
DROP TABLE tmp_region;

/* ============================================================ */
/* SECTION 34 : DCL -- users, grants, roles, synonyms              */
/* ============================================================ */
CREATE USER 'storeuser'@'%' IDENTIFIED BY 'sup3r_secret!';
GRANT SELECT, INSERT, UPDATE ON boutique.product TO 'storeuser'@'%' WITH GRANT OPTION;
GRANT SELECT ON boutique.category TO 'storeuser'@'%';
SHOW GRANTS FOR 'storeuser'@'%';
REVOKE UPDATE ON boutique.product FROM 'storeuser'@'%';
SHOW GRANTS FOR 'storeuser'@'%';

CREATE ROLE catalog_editor;
CREATE ROLE order_viewer;
CREATE ROLE store_admin;
SHOW ROLES;
GRANT ROLE catalog_editor TO 'storeuser'@'%';
GRANT ROLE order_viewer TO 'storeuser'@'%' WITH ADMIN OPTION;
REVOKE ROLE catalog_editor FROM 'storeuser'@'%';
DROP ROLE catalog_editor;
DROP ROLE IF EXISTS order_viewer;
DROP ROLE IF EXISTS store_admin;
SHOW ROLES;

CREATE SYNONYM prod_list FOR product;
CREATE OR REPLACE SYNONYM prod_list FOR product;
SHOW SYNONYMS;
SELECT id, sku, name FROM prod_list ORDER BY id LIMIT 3;
CREATE SYNONYM cust_list FOR customer;
SELECT id, full_name, tier FROM cust_list ORDER BY id LIMIT 3;
DROP SYNONYM prod_list;
DROP SYNONYM IF EXISTS cust_list;
SHOW SYNONYMS;

/* ============================================================ */
/* SECTION 35 : monitoring / maintenance                           */
/* ============================================================ */
CHECKPOINT;
VACUUM;
VACUUM product;
SHOW BUFFER POOL;
SHOW WAL;
SHOW LOCKS;
SHOW PROCESSLIST;
SHOW DATABASES;

/* ============================================================ */
/* SECTION 36 : data-type aliases                                  */
/* ============================================================ */
CREATE TABLE tmp_types (
    a INTEGER NOT NULL,
    b CHAR(20),
    c NUMERIC(8,2),
    d BOOL DEFAULT FALSE,
    e DECIMAL DEFAULT 0,
    f BLOB
);
INSERT INTO tmp_types (a,b,c,d,e,f) VALUES (1,'alpha',9.99,TRUE,150.00,'binary-ish-1');
INSERT INTO tmp_types (a,b,c,d,e,f) VALUES (2,'beta', 4.20,FALSE,  0.00,'binary-ish-2');
SELECT a, b, c, d, e, f FROM tmp_types ORDER BY a;
DROP TABLE tmp_types;

/* ============================================================ */
/* SECTION 37 : backup / restore                                   */
/* ============================================================ */
BACKUP DATABASE boutique INTO 'boutique_backup.sql';
RESTORE FROM 'boutique_backup.sql';

/* ============================================================ */
/* SECTION 38 : cleanup                                            */
/* ============================================================ */
DROP USER IF EXISTS 'storeuser'@'%';
DROP VIEW IF EXISTS v_active_products;
DROP VIEW IF EXISTS v_vip_customers;
DROP VIEW IF EXISTS v_low_stock;
DROP VIEW IF EXISTS v_order_summary;
DROP VIEW IF EXISTS v_top_suppliers;
DROP INDEX IF EXISTS idx_category_parent;
DROP INDEX IF EXISTS idx_product_category;
DROP INDEX IF EXISTS idx_product_supplier;
DROP INDEX IF EXISTS idx_product_status;
DROP INDEX IF EXISTS idx_product_cat_price;
DROP INDEX IF EXISTS idx_order_customer;
DROP INDEX IF EXISTS idx_order_status;
DROP INDEX IF EXISTS idx_order_item_order;
DROP INDEX IF EXISTS idx_review_product;
DROP INDEX IF EXISTS idx_product_sku;
DROP INDEX IF EXISTS idx_customer_email;
DROP INDEX IF EXISTS idx_product_price;
DROP TABLE IF EXISTS review;
DROP TABLE IF EXISTS order_item;
DROP TABLE IF EXISTS order_header;
DROP TABLE IF EXISTS inventory;
DROP TABLE IF EXISTS customer;
DROP TABLE IF EXISTS warehouse;
DROP TABLE IF EXISTS product;
DROP TABLE IF EXISTS supplier;
DROP TABLE IF EXISTS category;
DROP DATABASE boutique;
SHOW DATABASES;


