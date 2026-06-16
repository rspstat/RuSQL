RuSQL 주요 개념 정리


MVCC (Multi-Version Concurrency Control)
읽기와 쓰기가 서로 블로킹하지 않도록 데이터의 여러 버전을 유지하는 동시성 제어 기법.

Snapshot Isolation
트랜잭션 시작 시점의 스냅샷을 기준으로 읽어 Dirty Read와 Non-Repeatable Read를 방지하는 격리 수준.

SSI (Serializable Snapshot Isolation)
스냅샷 기반 실행 중 Write Skew 등 직렬화 이상을 감지하면 트랜잭션을 중단시키는 가장 강한 격리 수준.

2PL (Two-Phase Locking)
잠금 획득(Growing)과 잠금 해제(Shrinking) 두 단계로 나눠 직렬화 가능성을 보장하는 잠금 프로토콜.

행 잠금 (Row-level Lock)
테이블 전체가 아닌 특정 행 단위로 잠금을 걸어 동시 트랜잭션 충돌 범위를 최소화하는 기법.

GAP Lock
범위 조건 조회 시 존재하지 않는 키 사이의 간격에 잠금을 걸어 Phantom Read를 방지한다.

WAL (Write-Ahead Log)
데이터를 디스크에 쓰기 전에 로그를 먼저 기록해 장애 시 트랜잭션을 재실행하거나 롤백할 수 있게 한다.

Group Commit
여러 트랜잭션의 WAL flush를 묶어 한 번의 디스크 I/O로 처리해 COMMIT 처리량을 높이는 최적화.

ACID
트랜잭션이 보장해야 하는 원자성(Atomicity), 일관성(Consistency), 격리성(Isolation), 내구성(Durability).

XA 트랜잭션 (분산 트랜잭션)
여러 노드에 걸친 트랜잭션을 2PC(Two-Phase Commit)로 원자적으로 커밋하는 분산 프로토콜.

Buffer Pool
디스크에서 읽은 페이지를 메모리에 캐싱해 반복 접근 시 디스크 I/O를 줄이는 페이지 캐시.

B+Tree 인덱스
리프 노드에 실제 값을 저장하는 트리 구조로 포인트 조회와 범위 조회를 O(log N)에 처리한다.

Hash 인덱스
해시 함수로 키를 버킷에 매핑해 등호 조회를 O(1)에 처리한다. 범위 조회는 지원하지 않는다.

복합 인덱스 (Composite Index)
여러 컬럼을 묶어 하나의 인덱스로 구성. 선두 컬럼을 포함한 조건에서 인덱스 프루닝이 가능하다.

비용 기반 플래너 (Cost-Based Planner)
각 접근 경로의 비용을 추정해 가장 저렴한 실행 계획을 선택하는 쿼리 옵티마이저.

SeqScan
인덱스 없이 테이블의 모든 행을 순서대로 읽는 전체 스캔 방식.

파티션 프루닝 (Partition Pruning)
WHERE 조건에 해당하는 파티션만 스캔하고 나머지는 건너뛰어 I/O를 줄이는 최적화.

Sort-Merge Join
두 테이블을 조인 키 기준으로 정렬한 뒤 순차 병합하는 조인 알고리즘.

Hash Join
작은 쪽 테이블을 해시 테이블로 빌드한 뒤 큰 쪽을 프로브해 조인하는 알고리즘.

Nested Loop Join
외부 테이블의 각 행에 대해 내부 테이블을 반복 스캔하는 기본 조인 알고리즘.

LRU 쿼리 캐시
단순 SELECT 결과를 최대 512개 LRU 방식으로 캐싱하고 DML 발생 시 관련 항목을 즉시 무효화한다.

병렬 쿼리 실행 (Parallel Query)
GROUP BY, ORDER BY, SeqScan 필터 등을 rayon 멀티스레드로 병렬 처리해 대용량 쿼리 처리량을 높인다.

AST (Abstract Syntax Tree)
SQL 문자열을 파싱해 만드는 트리 구조로 Executor가 이를 순회하며 쿼리를 실행한다.

재귀 하강 파서 (Recursive Descent Parser)
문법 규칙 각각을 함수로 표현하고 재귀 호출로 SQL을 파싱하는 방식.

EXPLAIN / EXPLAIN ANALYZE
쿼리의 접근 경로, 조인 알고리즘, 예상 비용을 출력하거나 실제 실행 시간까지 함께 보여주는 진단 명령.

MySQL Wire Protocol
MySQL 클라이언트가 RuSQL에 직접 접속할 수 있도록 MySQL 4.1 이상 텍스트 프로토콜을 구현한 호환 레이어.

Savepoint
트랜잭션 내 중간 저장점을 설정해 전체 롤백 없이 특정 시점까지만 되돌릴 수 있게 하는 기능.

Deadlock Detection
두 트랜잭션이 서로의 잠금을 기다리는 교착 상태를 대기 그래프의 사이클로 감지하고 한쪽을 강제 중단시킨다.

VACUUM
MVCC로 인해 남아있는 삭제된 이전 버전 행들을 정리해 저장 공간을 회수하는 유지보수 작업.

Checkpoint
WAL 로그와 디스크 데이터 파일을 동기화하는 시점. 이전 WAL 로그를 제거 가능하게 해 복구 시간을 단축한다.

격리 수준 (Isolation Level)
READ COMMITTED, REPEATABLE READ, SERIALIZABLE 등 트랜잭션 간 가시성 범위를 단계별로 설정하는 옵션.

View
하나 이상의 테이블을 기반으로 정의된 가상 테이블. 쿼리를 이름으로 저장해 재사용하고 접근 제어에 활용한다.

Updatable View
단일 테이블 기반 뷰에 INSERT, UPDATE, DELETE를 직접 수행할 수 있도록 베이스 테이블로 자동 변환하는 기능.

Trigger
특정 테이블에 DML이 발생하기 전후(BEFORE/AFTER)에 자동으로 실행되는 SQL 로직.

Stored Procedure
서버에 저장해두고 CALL로 호출하는 SQL 프로시저. IF, WHILE, LOOP 등 제어 흐름을 포함할 수 있다.

사용자 정의 함수 (UDF)
SELECT 문 안에서 호출 가능한 사용자 정의 함수. 단일 값을 반환하며 표현식 내 자유롭게 사용된다.

CTE (Common Table Expression)
WITH 절로 쿼리 내에서 이름 붙인 임시 결과 집합을 정의해 가독성을 높이고 재사용하는 문법.

서브쿼리 (Subquery)
SELECT, WHERE, FROM 절 안에 중첩된 쿼리. 스칼라, 행, 테이블 서브쿼리로 나뉘며 상관 서브쿼리도 지원한다.

UNION / INTERSECT / EXCEPT
두 쿼리 결과를 합집합, 교집합, 차집합으로 결합하는 집합 연산자.

MERGE (Upsert)
대상 테이블과 소스를 비교해 일치하면 UPDATE, 불일치하면 INSERT를 수행하는 조건부 쓰기 명령.

RBAC (Role-Based Access Control)
사용자에게 역할(Role)을 부여하고 역할에 GRANT/REVOKE로 권한을 설정하는 접근 제어 모델.

Prepared Statement
SQL을 미리 파싱·컴파일해 두고 파라미터만 바꿔 반복 실행하는 방식. 파싱 오버헤드를 줄이고 SQL 인젝션을 방지한다.

ANALYZE TABLE
테이블의 행 수, 컬럼 분포 등 통계 정보를 수집해 비용 기반 플래너의 추정 정확도를 높이는 명령.

페이지 기반 스토리지 (Page-based Storage)
데이터를 고정 크기 페이지 단위로 디스크에 저장하고 관리하는 구조. Buffer Pool의 캐싱 단위이기도 하다.

Synonym
테이블이나 뷰에 별칭을 부여해 원본 이름을 노출하지 않거나 이름 변경 없이 다른 이름으로 접근하게 하는 객체.

Information Schema
데이터베이스 메타데이터(테이블 목록, 컬럼 정보, 인덱스 등)를 SQL로 조회할 수 있는 시스템 가상 테이블 집합.

Foreign Key (외래 키)
다른 테이블의 PK를 참조하는 컬럼 제약. ON DELETE CASCADE/RESTRICT로 참조 무결성을 자동으로 유지한다.

Auto Increment
INSERT 시 PK 값을 자동으로 증가 할당하는 기능. 시퀀스 충돌 없이 고유 식별자를 생성한다.


---


성능 측정 분석


Bulk DELETE TPS가 단순 DELETE보다 압도적으로 높은 이유

단순 DELETE는 WHERE id = X 조건으로 10,000번 개별 실행한다.
쿼리마다 네트워크 왕복, 파싱, WAL flush, B+Tree 포인트 조회가 각각 발생해 고정 비용이 10,000번 누적된다.

Bulk DELETE는 WHERE id BETWEEN lo AND hi 조건으로 200번만 실행한다.
B+Tree range_keys로 범위 내 PK를 한 번에 추출하고, 내림차순 정렬 후 swap_remove로 O(1) 제거를 500번 수행한다.
네트워크 왕복과 WAL flush가 50배 줄고, 새로운 메모리 할당 없이 기존 Vec 위치만 교체하므로 메모리 부담도 없다.

단순 DELETE가 PK 위치 인덱스로 O(1) swap_remove를 쓰는 빠른 경로임에도 불구하고,
쿼리 1건당 고정 비용이 10,000번 반복되면서 Bulk에 역전당하는 구조다.


SeqScan과 B+Tree 인덱스 TPS 차이가 76배인 이유

SeqScan은 5,000행 테이블 전체를 순서대로 읽으며 각 행을 WHERE 조건과 비교한다.
행 수에 비례하는 O(N) 작업이므로 테이블이 클수록 선형으로 느려진다.

B+Tree 인덱스는 루트에서 리프까지 O(log N) 노드만 탐색해 대상 행을 바로 찾는다.
5,000행 기준 트리 높이는 약 13단계로, 전체 스캔 대비 접근 횟수가 수백 배 적다.

또한 SeqScan은 조건에 맞지 않는 행도 모두 메모리에 올려 비교하는 반면,
인덱스는 조건을 만족하는 행의 위치만 추적하므로 불필요한 데이터 접근 자체가 없다.
이 두 요인이 겹쳐 76배 차이로 나타난다.


트랜잭션 TPS 차이 (AutoCommit 3,333 vs BEGIN/COMMIT 109)

AutoCommit은 각 INSERT가 묵시적으로 커밋되지만, 내부적으로 WAL flush 시점을 엔진이 최적화할 수 있다.
Group Commit이 적용되면 여러 autocommit 쓰기를 묶어 한 번의 flush로 처리해 처리량이 높아진다.

명시적 BEGIN/COMMIT은 COMMIT마다 WAL을 디스크에 강제 flush(fsync)한다.
1,000건 실행 시 COMMIT이 1,000번 발생하고 각각 디스크 동기화를 기다려야 하므로
처리량이 디스크 fsync 속도에 직접적으로 묶인다.

AutoCommit 대비 30배 차이는 비정상이 아니라 ACID 내구성(Durability) 보장의 비용이다.
MySQL도 innodb_flush_log_at_trx_commit=1 기본 설정에서 동일한 범위(100~500 TPS)가 나온다.
