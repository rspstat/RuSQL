-- ═══════════════════════════════════════════════════════════════════════════
-- RuSQL 발표 데모 · 벤치마크 + E-commerce + EXPLAIN ANALYZE
-- 실행: cat test/test_bench.sql | ./target/debug/rusql-cli.exe
-- ═══════════════════════════════════════════════════════════════════════════

DROP DATABASE IF EXISTS bench_demo;
CREATE DATABASE bench_demo;
USE bench_demo;

-- ═══════════════════════════════════════════════════════════════════════════
-- §1. 인덱스 성능 벤치마크 (500행 · 동일 데이터 · 인덱스 유무 차이)
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE sales_no_idx (
    id        INT AUTO INCREMENT,
    region_id INT,
    amount    INT,
    status    VARCHAR(20),
    PRIMARY KEY (id)
);

CREATE TABLE sales_idx (
    id        INT AUTO INCREMENT,
    region_id INT,
    amount    INT,
    status    VARCHAR(20),
    PRIMARY KEY (id)
);
CREATE INDEX idx_amount ON sales_idx (amount);
CREATE INDEX idx_region ON sales_idx (region_id);
CREATE INDEX idx_status ON sales_idx (status);

-- 500행 삽입 ── sales_no_idx (블록 1~5 각 100행)
INSERT INTO sales_no_idx (region_id, amount, status) VALUES
(1,8500,'paid'),(2,8500,'pending'),(3,8500,'shipped'),(4,8500,'cancelled'),(5,8500,'paid'),(6,8500,'pending'),(7,8500,'shipped'),(8,8500,'cancelled'),(9,8500,'paid'),(10,8500,'pending'),
(1,15200,'shipped'),(2,15200,'cancelled'),(3,15200,'paid'),(4,15200,'pending'),(5,15200,'shipped'),(6,15200,'cancelled'),(7,15200,'paid'),(8,15200,'pending'),(9,15200,'shipped'),(10,15200,'cancelled'),
(1,23800,'paid'),(2,23800,'pending'),(3,23800,'shipped'),(4,23800,'cancelled'),(5,23800,'paid'),(6,23800,'pending'),(7,23800,'shipped'),(8,23800,'cancelled'),(9,23800,'paid'),(10,23800,'pending'),
(1,34600,'shipped'),(2,34600,'cancelled'),(3,34600,'paid'),(4,34600,'pending'),(5,34600,'shipped'),(6,34600,'cancelled'),(7,34600,'paid'),(8,34600,'pending'),(9,34600,'shipped'),(10,34600,'cancelled'),
(1,47300,'paid'),(2,47300,'pending'),(3,47300,'shipped'),(4,47300,'cancelled'),(5,47300,'paid'),(6,47300,'pending'),(7,47300,'shipped'),(8,47300,'cancelled'),(9,47300,'paid'),(10,47300,'pending'),
(1,59800,'shipped'),(2,59800,'cancelled'),(3,59800,'paid'),(4,59800,'pending'),(5,59800,'shipped'),(6,59800,'cancelled'),(7,59800,'paid'),(8,59800,'pending'),(9,59800,'shipped'),(10,59800,'cancelled'),
(1,72100,'paid'),(2,72100,'pending'),(3,72100,'shipped'),(4,72100,'cancelled'),(5,72100,'paid'),(6,72100,'pending'),(7,72100,'shipped'),(8,72100,'cancelled'),(9,72100,'paid'),(10,72100,'pending'),
(1,85900,'shipped'),(2,85900,'cancelled'),(3,85900,'paid'),(4,85900,'pending'),(5,85900,'shipped'),(6,85900,'cancelled'),(7,85900,'paid'),(8,85900,'pending'),(9,85900,'shipped'),(10,85900,'cancelled'),
(1,95000,'paid'),(2,95000,'pending'),(3,95000,'shipped'),(4,95000,'cancelled'),(5,95000,'paid'),(6,95000,'pending'),(7,95000,'shipped'),(8,95000,'cancelled'),(9,95000,'paid'),(10,95000,'pending'),
(1,108400,'shipped'),(2,108400,'cancelled'),(3,108400,'paid'),(4,108400,'pending'),(5,108400,'shipped'),(6,108400,'cancelled'),(7,108400,'paid'),(8,108400,'pending'),(9,108400,'shipped'),(10,108400,'cancelled');

INSERT INTO sales_no_idx (region_id, amount, status) VALUES
(1,12300,'paid'),(2,12300,'pending'),(3,12300,'shipped'),(4,12300,'cancelled'),(5,12300,'paid'),(6,12300,'pending'),(7,12300,'shipped'),(8,12300,'cancelled'),(9,12300,'paid'),(10,12300,'pending'),
(1,19700,'shipped'),(2,19700,'cancelled'),(3,19700,'paid'),(4,19700,'pending'),(5,19700,'shipped'),(6,19700,'cancelled'),(7,19700,'paid'),(8,19700,'pending'),(9,19700,'shipped'),(10,19700,'cancelled'),
(1,28500,'paid'),(2,28500,'pending'),(3,28500,'shipped'),(4,28500,'cancelled'),(5,28500,'paid'),(6,28500,'pending'),(7,28500,'shipped'),(8,28500,'cancelled'),(9,28500,'paid'),(10,28500,'pending'),
(1,41200,'shipped'),(2,41200,'cancelled'),(3,41200,'paid'),(4,41200,'pending'),(5,41200,'shipped'),(6,41200,'cancelled'),(7,41200,'paid'),(8,41200,'pending'),(9,41200,'shipped'),(10,41200,'cancelled'),
(1,53700,'paid'),(2,53700,'pending'),(3,53700,'shipped'),(4,53700,'cancelled'),(5,53700,'paid'),(6,53700,'pending'),(7,53700,'shipped'),(8,53700,'cancelled'),(9,53700,'paid'),(10,53700,'pending'),
(1,66400,'shipped'),(2,66400,'cancelled'),(3,66400,'paid'),(4,66400,'pending'),(5,66400,'shipped'),(6,66400,'cancelled'),(7,66400,'paid'),(8,66400,'pending'),(9,66400,'shipped'),(10,66400,'cancelled'),
(1,79800,'paid'),(2,79800,'pending'),(3,79800,'shipped'),(4,79800,'cancelled'),(5,79800,'paid'),(6,79800,'pending'),(7,79800,'shipped'),(8,79800,'cancelled'),(9,79800,'paid'),(10,79800,'pending'),
(1,92500,'shipped'),(2,92500,'cancelled'),(3,92500,'paid'),(4,92500,'pending'),(5,92500,'shipped'),(6,92500,'cancelled'),(7,92500,'paid'),(8,92500,'pending'),(9,92500,'shipped'),(10,92500,'cancelled'),
(1,105600,'paid'),(2,105600,'pending'),(3,105600,'shipped'),(4,105600,'cancelled'),(5,105600,'paid'),(6,105600,'pending'),(7,105600,'shipped'),(8,105600,'cancelled'),(9,105600,'paid'),(10,105600,'pending'),
(1,118900,'shipped'),(2,118900,'cancelled'),(3,118900,'paid'),(4,118900,'pending'),(5,118900,'shipped'),(6,118900,'cancelled'),(7,118900,'paid'),(8,118900,'pending'),(9,118900,'shipped'),(10,118900,'cancelled');

INSERT INTO sales_no_idx (region_id, amount, status) VALUES
(1,125000,'paid'),(2,125000,'pending'),(3,125000,'shipped'),(4,125000,'cancelled'),(5,125000,'paid'),(6,125000,'pending'),(7,125000,'shipped'),(8,125000,'cancelled'),(9,125000,'paid'),(10,125000,'pending'),
(1,138500,'shipped'),(2,138500,'cancelled'),(3,138500,'paid'),(4,138500,'pending'),(5,138500,'shipped'),(6,138500,'cancelled'),(7,138500,'paid'),(8,138500,'pending'),(9,138500,'shipped'),(10,138500,'cancelled'),
(1,152000,'paid'),(2,152000,'pending'),(3,152000,'shipped'),(4,152000,'cancelled'),(5,152000,'paid'),(6,152000,'pending'),(7,152000,'shipped'),(8,152000,'cancelled'),(9,152000,'paid'),(10,152000,'pending'),
(1,167800,'shipped'),(2,167800,'cancelled'),(3,167800,'paid'),(4,167800,'pending'),(5,167800,'shipped'),(6,167800,'cancelled'),(7,167800,'paid'),(8,167800,'pending'),(9,167800,'shipped'),(10,167800,'cancelled'),
(1,183400,'paid'),(2,183400,'pending'),(3,183400,'shipped'),(4,183400,'cancelled'),(5,183400,'paid'),(6,183400,'pending'),(7,183400,'shipped'),(8,183400,'cancelled'),(9,183400,'paid'),(10,183400,'pending'),
(1,198700,'shipped'),(2,198700,'cancelled'),(3,198700,'paid'),(4,198700,'pending'),(5,198700,'shipped'),(6,198700,'cancelled'),(7,198700,'paid'),(8,198700,'pending'),(9,198700,'shipped'),(10,198700,'cancelled'),
(1,215300,'paid'),(2,215300,'pending'),(3,215300,'shipped'),(4,215300,'cancelled'),(5,215300,'paid'),(6,215300,'pending'),(7,215300,'shipped'),(8,215300,'cancelled'),(9,215300,'paid'),(10,215300,'pending'),
(1,231800,'shipped'),(2,231800,'cancelled'),(3,231800,'paid'),(4,231800,'pending'),(5,231800,'shipped'),(6,231800,'cancelled'),(7,231800,'paid'),(8,231800,'pending'),(9,231800,'shipped'),(10,231800,'cancelled'),
(1,248500,'paid'),(2,248500,'pending'),(3,248500,'shipped'),(4,248500,'cancelled'),(5,248500,'paid'),(6,248500,'pending'),(7,248500,'shipped'),(8,248500,'cancelled'),(9,248500,'paid'),(10,248500,'pending'),
(1,265100,'shipped'),(2,265100,'cancelled'),(3,265100,'paid'),(4,265100,'pending'),(5,265100,'shipped'),(6,265100,'cancelled'),(7,265100,'paid'),(8,265100,'pending'),(9,265100,'shipped'),(10,265100,'cancelled');

INSERT INTO sales_no_idx (region_id, amount, status) VALUES
(1,280600,'paid'),(2,280600,'pending'),(3,280600,'shipped'),(4,280600,'cancelled'),(5,280600,'paid'),(6,280600,'pending'),(7,280600,'shipped'),(8,280600,'cancelled'),(9,280600,'paid'),(10,280600,'pending'),
(1,296300,'shipped'),(2,296300,'cancelled'),(3,296300,'paid'),(4,296300,'pending'),(5,296300,'shipped'),(6,296300,'cancelled'),(7,296300,'paid'),(8,296300,'pending'),(9,296300,'shipped'),(10,296300,'cancelled'),
(1,313800,'paid'),(2,313800,'pending'),(3,313800,'shipped'),(4,313800,'cancelled'),(5,313800,'paid'),(6,313800,'pending'),(7,313800,'shipped'),(8,313800,'cancelled'),(9,313800,'paid'),(10,313800,'pending'),
(1,330500,'shipped'),(2,330500,'cancelled'),(3,330500,'paid'),(4,330500,'pending'),(5,330500,'shipped'),(6,330500,'cancelled'),(7,330500,'paid'),(8,330500,'pending'),(9,330500,'shipped'),(10,330500,'cancelled'),
(1,347200,'paid'),(2,347200,'pending'),(3,347200,'shipped'),(4,347200,'cancelled'),(5,347200,'paid'),(6,347200,'pending'),(7,347200,'shipped'),(8,347200,'cancelled'),(9,347200,'paid'),(10,347200,'pending'),
(1,363900,'shipped'),(2,363900,'cancelled'),(3,363900,'paid'),(4,363900,'pending'),(5,363900,'shipped'),(6,363900,'cancelled'),(7,363900,'paid'),(8,363900,'pending'),(9,363900,'shipped'),(10,363900,'cancelled'),
(1,381400,'paid'),(2,381400,'pending'),(3,381400,'shipped'),(4,381400,'cancelled'),(5,381400,'paid'),(6,381400,'pending'),(7,381400,'shipped'),(8,381400,'cancelled'),(9,381400,'paid'),(10,381400,'pending'),
(1,398100,'shipped'),(2,398100,'cancelled'),(3,398100,'paid'),(4,398100,'pending'),(5,398100,'shipped'),(6,398100,'cancelled'),(7,398100,'paid'),(8,398100,'pending'),(9,398100,'shipped'),(10,398100,'cancelled'),
(1,415600,'paid'),(2,415600,'pending'),(3,415600,'shipped'),(4,415600,'cancelled'),(5,415600,'paid'),(6,415600,'pending'),(7,415600,'shipped'),(8,415600,'cancelled'),(9,415600,'paid'),(10,415600,'pending'),
(1,432300,'shipped'),(2,432300,'cancelled'),(3,432300,'paid'),(4,432300,'pending'),(5,432300,'shipped'),(6,432300,'cancelled'),(7,432300,'paid'),(8,432300,'pending'),(9,432300,'shipped'),(10,432300,'cancelled');

INSERT INTO sales_no_idx (region_id, amount, status) VALUES
(1,16800,'paid'),(2,16800,'pending'),(3,16800,'shipped'),(4,16800,'cancelled'),(5,16800,'paid'),(6,16800,'pending'),(7,16800,'shipped'),(8,16800,'cancelled'),(9,16800,'paid'),(10,16800,'pending'),
(1,29400,'shipped'),(2,29400,'cancelled'),(3,29400,'paid'),(4,29400,'pending'),(5,29400,'shipped'),(6,29400,'cancelled'),(7,29400,'paid'),(8,29400,'pending'),(9,29400,'shipped'),(10,29400,'cancelled'),
(1,42900,'paid'),(2,42900,'pending'),(3,42900,'shipped'),(4,42900,'cancelled'),(5,42900,'paid'),(6,42900,'pending'),(7,42900,'shipped'),(8,42900,'cancelled'),(9,42900,'paid'),(10,42900,'pending'),
(1,56300,'shipped'),(2,56300,'cancelled'),(3,56300,'paid'),(4,56300,'pending'),(5,56300,'shipped'),(6,56300,'cancelled'),(7,56300,'paid'),(8,56300,'pending'),(9,56300,'shipped'),(10,56300,'cancelled'),
(1,71700,'paid'),(2,71700,'pending'),(3,71700,'shipped'),(4,71700,'cancelled'),(5,71700,'paid'),(6,71700,'pending'),(7,71700,'shipped'),(8,71700,'cancelled'),(9,71700,'paid'),(10,71700,'pending'),
(1,87200,'shipped'),(2,87200,'cancelled'),(3,87200,'paid'),(4,87200,'pending'),(5,87200,'shipped'),(6,87200,'cancelled'),(7,87200,'paid'),(8,87200,'pending'),(9,87200,'shipped'),(10,87200,'cancelled'),
(1,99500,'paid'),(2,99500,'pending'),(3,99500,'shipped'),(4,99500,'cancelled'),(5,99500,'paid'),(6,99500,'pending'),(7,99500,'shipped'),(8,99500,'cancelled'),(9,99500,'paid'),(10,99500,'pending'),
(1,115000,'shipped'),(2,115000,'cancelled'),(3,115000,'paid'),(4,115000,'pending'),(5,115000,'shipped'),(6,115000,'cancelled'),(7,115000,'paid'),(8,115000,'pending'),(9,115000,'shipped'),(10,115000,'cancelled'),
(1,143500,'paid'),(2,143500,'pending'),(3,143500,'shipped'),(4,143500,'cancelled'),(5,143500,'paid'),(6,143500,'pending'),(7,143500,'shipped'),(8,143500,'cancelled'),(9,143500,'paid'),(10,143500,'pending'),
(1,178900,'shipped'),(2,178900,'cancelled'),(3,178900,'paid'),(4,178900,'pending'),(5,178900,'shipped'),(6,178900,'cancelled'),(7,178900,'paid'),(8,178900,'pending'),(9,178900,'shipped'),(10,178900,'cancelled');

-- 500행 삽입 ── sales_idx (동일 데이터)
INSERT INTO sales_idx (region_id, amount, status) VALUES
(1,8500,'paid'),(2,8500,'pending'),(3,8500,'shipped'),(4,8500,'cancelled'),(5,8500,'paid'),(6,8500,'pending'),(7,8500,'shipped'),(8,8500,'cancelled'),(9,8500,'paid'),(10,8500,'pending'),
(1,15200,'shipped'),(2,15200,'cancelled'),(3,15200,'paid'),(4,15200,'pending'),(5,15200,'shipped'),(6,15200,'cancelled'),(7,15200,'paid'),(8,15200,'pending'),(9,15200,'shipped'),(10,15200,'cancelled'),
(1,23800,'paid'),(2,23800,'pending'),(3,23800,'shipped'),(4,23800,'cancelled'),(5,23800,'paid'),(6,23800,'pending'),(7,23800,'shipped'),(8,23800,'cancelled'),(9,23800,'paid'),(10,23800,'pending'),
(1,34600,'shipped'),(2,34600,'cancelled'),(3,34600,'paid'),(4,34600,'pending'),(5,34600,'shipped'),(6,34600,'cancelled'),(7,34600,'paid'),(8,34600,'pending'),(9,34600,'shipped'),(10,34600,'cancelled'),
(1,47300,'paid'),(2,47300,'pending'),(3,47300,'shipped'),(4,47300,'cancelled'),(5,47300,'paid'),(6,47300,'pending'),(7,47300,'shipped'),(8,47300,'cancelled'),(9,47300,'paid'),(10,47300,'pending'),
(1,59800,'shipped'),(2,59800,'cancelled'),(3,59800,'paid'),(4,59800,'pending'),(5,59800,'shipped'),(6,59800,'cancelled'),(7,59800,'paid'),(8,59800,'pending'),(9,59800,'shipped'),(10,59800,'cancelled'),
(1,72100,'paid'),(2,72100,'pending'),(3,72100,'shipped'),(4,72100,'cancelled'),(5,72100,'paid'),(6,72100,'pending'),(7,72100,'shipped'),(8,72100,'cancelled'),(9,72100,'paid'),(10,72100,'pending'),
(1,85900,'shipped'),(2,85900,'cancelled'),(3,85900,'paid'),(4,85900,'pending'),(5,85900,'shipped'),(6,85900,'cancelled'),(7,85900,'paid'),(8,85900,'pending'),(9,85900,'shipped'),(10,85900,'cancelled'),
(1,95000,'paid'),(2,95000,'pending'),(3,95000,'shipped'),(4,95000,'cancelled'),(5,95000,'paid'),(6,95000,'pending'),(7,95000,'shipped'),(8,95000,'cancelled'),(9,95000,'paid'),(10,95000,'pending'),
(1,108400,'shipped'),(2,108400,'cancelled'),(3,108400,'paid'),(4,108400,'pending'),(5,108400,'shipped'),(6,108400,'cancelled'),(7,108400,'paid'),(8,108400,'pending'),(9,108400,'shipped'),(10,108400,'cancelled');

INSERT INTO sales_idx (region_id, amount, status) VALUES
(1,12300,'paid'),(2,12300,'pending'),(3,12300,'shipped'),(4,12300,'cancelled'),(5,12300,'paid'),(6,12300,'pending'),(7,12300,'shipped'),(8,12300,'cancelled'),(9,12300,'paid'),(10,12300,'pending'),
(1,19700,'shipped'),(2,19700,'cancelled'),(3,19700,'paid'),(4,19700,'pending'),(5,19700,'shipped'),(6,19700,'cancelled'),(7,19700,'paid'),(8,19700,'pending'),(9,19700,'shipped'),(10,19700,'cancelled'),
(1,28500,'paid'),(2,28500,'pending'),(3,28500,'shipped'),(4,28500,'cancelled'),(5,28500,'paid'),(6,28500,'pending'),(7,28500,'shipped'),(8,28500,'cancelled'),(9,28500,'paid'),(10,28500,'pending'),
(1,41200,'shipped'),(2,41200,'cancelled'),(3,41200,'paid'),(4,41200,'pending'),(5,41200,'shipped'),(6,41200,'cancelled'),(7,41200,'paid'),(8,41200,'pending'),(9,41200,'shipped'),(10,41200,'cancelled'),
(1,53700,'paid'),(2,53700,'pending'),(3,53700,'shipped'),(4,53700,'cancelled'),(5,53700,'paid'),(6,53700,'pending'),(7,53700,'shipped'),(8,53700,'cancelled'),(9,53700,'paid'),(10,53700,'pending'),
(1,66400,'shipped'),(2,66400,'cancelled'),(3,66400,'paid'),(4,66400,'pending'),(5,66400,'shipped'),(6,66400,'cancelled'),(7,66400,'paid'),(8,66400,'pending'),(9,66400,'shipped'),(10,66400,'cancelled'),
(1,79800,'paid'),(2,79800,'pending'),(3,79800,'shipped'),(4,79800,'cancelled'),(5,79800,'paid'),(6,79800,'pending'),(7,79800,'shipped'),(8,79800,'cancelled'),(9,79800,'paid'),(10,79800,'pending'),
(1,92500,'shipped'),(2,92500,'cancelled'),(3,92500,'paid'),(4,92500,'pending'),(5,92500,'shipped'),(6,92500,'cancelled'),(7,92500,'paid'),(8,92500,'pending'),(9,92500,'shipped'),(10,92500,'cancelled'),
(1,105600,'paid'),(2,105600,'pending'),(3,105600,'shipped'),(4,105600,'cancelled'),(5,105600,'paid'),(6,105600,'pending'),(7,105600,'shipped'),(8,105600,'cancelled'),(9,105600,'paid'),(10,105600,'pending'),
(1,118900,'shipped'),(2,118900,'cancelled'),(3,118900,'paid'),(4,118900,'pending'),(5,118900,'shipped'),(6,118900,'cancelled'),(7,118900,'paid'),(8,118900,'pending'),(9,118900,'shipped'),(10,118900,'cancelled');

INSERT INTO sales_idx (region_id, amount, status) VALUES
(1,125000,'paid'),(2,125000,'pending'),(3,125000,'shipped'),(4,125000,'cancelled'),(5,125000,'paid'),(6,125000,'pending'),(7,125000,'shipped'),(8,125000,'cancelled'),(9,125000,'paid'),(10,125000,'pending'),
(1,138500,'shipped'),(2,138500,'cancelled'),(3,138500,'paid'),(4,138500,'pending'),(5,138500,'shipped'),(6,138500,'cancelled'),(7,138500,'paid'),(8,138500,'pending'),(9,138500,'shipped'),(10,138500,'cancelled'),
(1,152000,'paid'),(2,152000,'pending'),(3,152000,'shipped'),(4,152000,'cancelled'),(5,152000,'paid'),(6,152000,'pending'),(7,152000,'shipped'),(8,152000,'cancelled'),(9,152000,'paid'),(10,152000,'pending'),
(1,167800,'shipped'),(2,167800,'cancelled'),(3,167800,'paid'),(4,167800,'pending'),(5,167800,'shipped'),(6,167800,'cancelled'),(7,167800,'paid'),(8,167800,'pending'),(9,167800,'shipped'),(10,167800,'cancelled'),
(1,183400,'paid'),(2,183400,'pending'),(3,183400,'shipped'),(4,183400,'cancelled'),(5,183400,'paid'),(6,183400,'pending'),(7,183400,'shipped'),(8,183400,'cancelled'),(9,183400,'paid'),(10,183400,'pending'),
(1,198700,'shipped'),(2,198700,'cancelled'),(3,198700,'paid'),(4,198700,'pending'),(5,198700,'shipped'),(6,198700,'cancelled'),(7,198700,'paid'),(8,198700,'pending'),(9,198700,'shipped'),(10,198700,'cancelled'),
(1,215300,'paid'),(2,215300,'pending'),(3,215300,'shipped'),(4,215300,'cancelled'),(5,215300,'paid'),(6,215300,'pending'),(7,215300,'shipped'),(8,215300,'cancelled'),(9,215300,'paid'),(10,215300,'pending'),
(1,231800,'shipped'),(2,231800,'cancelled'),(3,231800,'paid'),(4,231800,'pending'),(5,231800,'shipped'),(6,231800,'cancelled'),(7,231800,'paid'),(8,231800,'pending'),(9,231800,'shipped'),(10,231800,'cancelled'),
(1,248500,'paid'),(2,248500,'pending'),(3,248500,'shipped'),(4,248500,'cancelled'),(5,248500,'paid'),(6,248500,'pending'),(7,248500,'shipped'),(8,248500,'cancelled'),(9,248500,'paid'),(10,248500,'pending'),
(1,265100,'shipped'),(2,265100,'cancelled'),(3,265100,'paid'),(4,265100,'pending'),(5,265100,'shipped'),(6,265100,'cancelled'),(7,265100,'paid'),(8,265100,'pending'),(9,265100,'shipped'),(10,265100,'cancelled');

INSERT INTO sales_idx (region_id, amount, status) VALUES
(1,280600,'paid'),(2,280600,'pending'),(3,280600,'shipped'),(4,280600,'cancelled'),(5,280600,'paid'),(6,280600,'pending'),(7,280600,'shipped'),(8,280600,'cancelled'),(9,280600,'paid'),(10,280600,'pending'),
(1,296300,'shipped'),(2,296300,'cancelled'),(3,296300,'paid'),(4,296300,'pending'),(5,296300,'shipped'),(6,296300,'cancelled'),(7,296300,'paid'),(8,296300,'pending'),(9,296300,'shipped'),(10,296300,'cancelled'),
(1,313800,'paid'),(2,313800,'pending'),(3,313800,'shipped'),(4,313800,'cancelled'),(5,313800,'paid'),(6,313800,'pending'),(7,313800,'shipped'),(8,313800,'cancelled'),(9,313800,'paid'),(10,313800,'pending'),
(1,330500,'shipped'),(2,330500,'cancelled'),(3,330500,'paid'),(4,330500,'pending'),(5,330500,'shipped'),(6,330500,'cancelled'),(7,330500,'paid'),(8,330500,'pending'),(9,330500,'shipped'),(10,330500,'cancelled'),
(1,347200,'paid'),(2,347200,'pending'),(3,347200,'shipped'),(4,347200,'cancelled'),(5,347200,'paid'),(6,347200,'pending'),(7,347200,'shipped'),(8,347200,'cancelled'),(9,347200,'paid'),(10,347200,'pending'),
(1,363900,'shipped'),(2,363900,'cancelled'),(3,363900,'paid'),(4,363900,'pending'),(5,363900,'shipped'),(6,363900,'cancelled'),(7,363900,'paid'),(8,363900,'pending'),(9,363900,'shipped'),(10,363900,'cancelled'),
(1,381400,'paid'),(2,381400,'pending'),(3,381400,'shipped'),(4,381400,'cancelled'),(5,381400,'paid'),(6,381400,'pending'),(7,381400,'shipped'),(8,381400,'cancelled'),(9,381400,'paid'),(10,381400,'pending'),
(1,398100,'shipped'),(2,398100,'cancelled'),(3,398100,'paid'),(4,398100,'pending'),(5,398100,'shipped'),(6,398100,'cancelled'),(7,398100,'paid'),(8,398100,'pending'),(9,398100,'shipped'),(10,398100,'cancelled'),
(1,415600,'paid'),(2,415600,'pending'),(3,415600,'shipped'),(4,415600,'cancelled'),(5,415600,'paid'),(6,415600,'pending'),(7,415600,'shipped'),(8,415600,'cancelled'),(9,415600,'paid'),(10,415600,'pending'),
(1,432300,'shipped'),(2,432300,'cancelled'),(3,432300,'paid'),(4,432300,'pending'),(5,432300,'shipped'),(6,432300,'cancelled'),(7,432300,'paid'),(8,432300,'pending'),(9,432300,'shipped'),(10,432300,'cancelled');

INSERT INTO sales_idx (region_id, amount, status) VALUES
(1,16800,'paid'),(2,16800,'pending'),(3,16800,'shipped'),(4,16800,'cancelled'),(5,16800,'paid'),(6,16800,'pending'),(7,16800,'shipped'),(8,16800,'cancelled'),(9,16800,'paid'),(10,16800,'pending'),
(1,29400,'shipped'),(2,29400,'cancelled'),(3,29400,'paid'),(4,29400,'pending'),(5,29400,'shipped'),(6,29400,'cancelled'),(7,29400,'paid'),(8,29400,'pending'),(9,29400,'shipped'),(10,29400,'cancelled'),
(1,42900,'paid'),(2,42900,'pending'),(3,42900,'shipped'),(4,42900,'cancelled'),(5,42900,'paid'),(6,42900,'pending'),(7,42900,'shipped'),(8,42900,'cancelled'),(9,42900,'paid'),(10,42900,'pending'),
(1,56300,'shipped'),(2,56300,'cancelled'),(3,56300,'paid'),(4,56300,'pending'),(5,56300,'shipped'),(6,56300,'cancelled'),(7,56300,'paid'),(8,56300,'pending'),(9,56300,'shipped'),(10,56300,'cancelled'),
(1,71700,'paid'),(2,71700,'pending'),(3,71700,'shipped'),(4,71700,'cancelled'),(5,71700,'paid'),(6,71700,'pending'),(7,71700,'shipped'),(8,71700,'cancelled'),(9,71700,'paid'),(10,71700,'pending'),
(1,87200,'shipped'),(2,87200,'cancelled'),(3,87200,'paid'),(4,87200,'pending'),(5,87200,'shipped'),(6,87200,'cancelled'),(7,87200,'paid'),(8,87200,'pending'),(9,87200,'shipped'),(10,87200,'cancelled'),
(1,99500,'paid'),(2,99500,'pending'),(3,99500,'shipped'),(4,99500,'cancelled'),(5,99500,'paid'),(6,99500,'pending'),(7,99500,'shipped'),(8,99500,'cancelled'),(9,99500,'paid'),(10,99500,'pending'),
(1,115000,'shipped'),(2,115000,'cancelled'),(3,115000,'paid'),(4,115000,'pending'),(5,115000,'shipped'),(6,115000,'cancelled'),(7,115000,'paid'),(8,115000,'pending'),(9,115000,'shipped'),(10,115000,'cancelled'),
(1,143500,'paid'),(2,143500,'pending'),(3,143500,'shipped'),(4,143500,'cancelled'),(5,143500,'paid'),(6,143500,'pending'),(7,143500,'shipped'),(8,143500,'cancelled'),(9,143500,'paid'),(10,143500,'pending'),
(1,178900,'shipped'),(2,178900,'cancelled'),(3,178900,'paid'),(4,178900,'pending'),(5,178900,'shipped'),(6,178900,'cancelled'),(7,178900,'paid'),(8,178900,'pending'),(9,178900,'shipped'),(10,178900,'cancelled');

-- ─── §1-A  플래너 비교: Seq Scan vs Index Scan ───────────────────────────
EXPLAIN SELECT id, amount FROM sales_no_idx WHERE amount = 95000;
EXPLAIN SELECT id, amount FROM sales_idx     WHERE amount = 95000;

-- ─── §1-B  포인트 검색 (같은 WHERE, 인덱스 유무 차이) ───────────────────
SELECT id, region_id, amount FROM sales_no_idx WHERE amount = 95000;
SELECT id, region_id, amount FROM sales_idx     WHERE amount = 95000;

-- ─── §1-C  범위 검색 COUNT ───────────────────────────────────────────────
EXPLAIN SELECT COUNT(*) FROM sales_no_idx WHERE amount BETWEEN 100000 AND 300000;
EXPLAIN SELECT COUNT(*) FROM sales_idx     WHERE amount BETWEEN 100000 AND 300000;
SELECT COUNT(*) FROM sales_no_idx WHERE amount BETWEEN 100000 AND 300000;
SELECT COUNT(*) FROM sales_idx     WHERE amount BETWEEN 100000 AND 300000;

-- ─── §1-D  Top-K 인덱스 최적화: ORDER BY + LIMIT ────────────────────────
-- amount >= 200000 인 행 중 가장 큰 5건 — 별도 정렬 없이 인덱스 역순 사용
EXPLAIN SELECT region_id, amount FROM sales_idx WHERE amount >= 200000 ORDER BY amount DESC LIMIT 5;
SELECT  region_id, amount FROM sales_idx WHERE amount >= 200000 ORDER BY amount DESC LIMIT 5;

-- ═══════════════════════════════════════════════════════════════════════════
-- §2. E-commerce 데모 데이터베이스
-- ═══════════════════════════════════════════════════════════════════════════

CREATE TABLE customers (
    id    INT AUTO INCREMENT,
    name  VARCHAR(50),
    city  VARCHAR(30),
    grade VARCHAR(10),
    PRIMARY KEY (id)
);

CREATE TABLE products (
    id       INT AUTO INCREMENT,
    name     VARCHAR(60),
    category VARCHAR(30),
    price    INT,
    PRIMARY KEY (id)
);

CREATE TABLE orders (
    id          INT AUTO INCREMENT,
    customer_id INT,
    product_id  INT,
    qty         INT,
    amount      INT,
    status      VARCHAR(20),
    PRIMARY KEY (id)
);
CREATE INDEX idx_ord_cust   ON orders (customer_id);
CREATE INDEX idx_ord_amount ON orders (amount);
CREATE INDEX idx_ord_status ON orders (status);

INSERT INTO customers (name, city, grade) VALUES
('김민준','서울','VIP'),('이서연','부산','Regular'),('박지호','인천','VIP'),
('최수아','대구','Regular'),('정현우','광주','Premium'),('강나은','대전','Regular'),
('윤서준','울산','Premium'),('임지아','수원','VIP'),('한도윤','창원','Regular'),
('오채원','전주','Premium');

INSERT INTO products (name, category, price) VALUES
('노트북','전자기기',850000),('스마트폰','전자기기',650000),
('태블릿','전자기기',480000),('무선이어폰','전자기기',185000),
('청바지','의류',65000),('티셔츠','의류',29000),('운동화','신발',89000),
('커피메이커','생활가전',125000),('소파','가구',450000),('가방','잡화',95000);

INSERT INTO orders (customer_id, product_id, qty, amount, status) VALUES
(1,1,1,850000,'paid'),(1,4,1,185000,'paid'),(1,7,2,178000,'shipped'),
(2,5,3,195000,'paid'),(2,6,5,145000,'paid'),(2,10,1,95000,'cancelled'),
(3,2,1,650000,'paid'),(3,1,2,1700000,'paid'),(3,4,1,185000,'shipped'),
(4,6,4,116000,'pending'),(4,7,1,89000,'paid'),(4,8,1,125000,'paid'),
(5,3,1,480000,'paid'),(5,9,1,450000,'shipped'),(5,2,1,650000,'paid'),
(6,5,2,130000,'paid'),(6,10,2,190000,'paid'),(6,1,1,850000,'paid'),
(7,1,1,850000,'paid'),(7,4,2,370000,'paid'),(7,8,1,125000,'pending'),
(8,2,1,650000,'paid'),(8,3,1,480000,'shipped'),(8,5,4,260000,'paid'),
(9,6,2,58000,'paid'),(9,7,1,89000,'paid'),(9,10,1,95000,'cancelled'),
(10,4,1,185000,'paid'),(10,1,1,850000,'paid'),(10,9,1,450000,'shipped'),
(1,2,1,650000,'paid'),(2,1,1,850000,'shipped'),(3,7,1,89000,'paid'),
(4,4,1,185000,'paid'),(5,10,2,190000,'shipped'),(6,3,1,480000,'paid'),
(7,3,1,480000,'paid'),(8,8,2,250000,'paid'),(9,2,1,650000,'paid'),
(10,6,5,145000,'paid'),(1,9,1,450000,'paid'),(2,4,2,370000,'shipped'),
(3,8,1,125000,'paid'),(4,2,1,650000,'paid'),(5,1,1,850000,'paid'),
(6,7,1,89000,'paid'),(7,10,1,95000,'cancelled'),(8,1,1,850000,'paid'),
(9,1,1,850000,'shipped'),(10,5,3,87000,'paid');

-- ─── §2-A  고객별 총 구매금액 TOP 5 (JOIN + GROUP BY) ───────────────────
SELECT c.name, c.city, c.grade,
       COUNT(*) AS order_count,
       SUM(o.amount) AS total_spent
FROM customers c JOIN orders o ON c.id = o.customer_id
WHERE o.status = 'paid'
GROUP BY c.name, c.city, c.grade
ORDER BY total_spent DESC
LIMIT 5;

-- ─── §2-B  카테고리별 매출 (JOIN + GROUP BY) ─────────────────────────────
SELECT p.category,
       COUNT(*) AS order_count,
       SUM(o.amount) AS revenue
FROM products p JOIN orders o ON p.id = o.product_id
WHERE o.status = 'paid'
GROUP BY p.category
ORDER BY revenue DESC;

-- ─── §2-C  고객별 구매 순위 (WINDOW FUNCTION) ────────────────────────────
SELECT customer_id, amount,
       ROW_NUMBER() OVER (PARTITION BY customer_id ORDER BY amount DESC) AS rank_in_customer
FROM orders
WHERE status = 'paid'
ORDER BY customer_id, rank_in_customer
LIMIT 15;

-- ─── §2-D  Top-K: 최고금액 주문 3건 ─────────────────────────────────────
EXPLAIN SELECT id, customer_id, amount FROM orders WHERE amount >= 1 ORDER BY amount DESC LIMIT 3;
SELECT  id, customer_id, amount FROM orders WHERE amount >= 1 ORDER BY amount DESC LIMIT 3;

-- ═══════════════════════════════════════════════════════════════════════════
-- §3. EXPLAIN ANALYZE — 통계 수집 전후 정확도 비교
-- ═══════════════════════════════════════════════════════════════════════════

-- 3-A  통계 없을 때: 옵티마이저 추정 부정확
EXPLAIN ANALYZE SELECT id, amount FROM sales_idx WHERE amount >= 200000;

-- 3-B  통계 수집 (histogram 생성)
ANALYZE TABLE sales_idx;

-- 3-C  통계 있을 때: 추정 정확도 향상
EXPLAIN ANALYZE SELECT id, amount FROM sales_idx WHERE amount >= 200000;

-- 3-D  범위 쿼리 ANALYZE
EXPLAIN ANALYZE SELECT id, amount FROM sales_idx WHERE amount BETWEEN 100000 AND 300000;

-- 3-E  JOIN 쿼리 EXPLAIN ANALYZE
EXPLAIN ANALYZE
SELECT c.name, COUNT(*) AS cnt, SUM(o.amount) AS total
FROM customers c JOIN orders o ON c.id = o.customer_id
GROUP BY c.name
ORDER BY total DESC;

-- ═══════════════════════════════════════════════════════════════════════════
-- 정리
-- ═══════════════════════════════════════════════════════════════════════════
DROP DATABASE bench_demo;
