# RuSQL 지원 SQL 문법 전체 목록

RuSQL이 실제로 받아서 실행하는 SQL 문법을 한 줄씩 정리한 문서입니다. 줄마다 `문법 : 맡은 기능` 형식이고, 문법 앞뒤의 `[ ]`는 생략할 수 있다는 뜻입니다.
목록의 모든 문법은 실행 중인 엔진에 한 줄씩 넣어 보고 받아들여지는 것만 적었습니다. 받아들이지 않는 문법은 맨 끝 **16. 지원하지 않는 문법**에 따로 모았습니다.

- **네이티브 프로토콜**(앱 · MCP · CLI)과 **MySQL 프로토콜**(mysql 클라이언트 · 드라이버)은 같은 엔진을 씁니다. 한쪽에서만 되는 문법은 **14. MySQL 프로토콜에서만 되는 문법**에 있습니다.
- 이름(DB · 테이블 · 컬럼)은 대소문자를 구분하지 않고, `` `이름` ``으로 감싸면 예약어도 이름으로 쓸 수 있고, `db.table` · `db.table.column`으로 DB를 붙여 쓸 수 있습니다.
- 문장 끝의 `;`는 생략할 수 있고, 텍스트는 `utf8mb4_0900_ai_ci`(대소문자 · 악센트 무시)로 비교합니다.

---

## 1. 데이터베이스

- CREATE DATABASE [IF NOT EXISTS] db : 데이터베이스 생성
- CREATE SCHEMA [IF NOT EXISTS] db : CREATE DATABASE와 같음
- CREATE DATABASE db [DEFAULT] CHARACTER SET / CHARSET / COLLATE ... : 문자 집합 · 정렬 규칙을 적어서 생성(utf8mb4 계열 · `_ci` 정렬만 받음)
- DROP DATABASE [IF EXISTS] db : 데이터베이스 삭제
- DROP SCHEMA db : DROP DATABASE와 같음
- USE db : 사용할 데이터베이스 선택
- USE DATABASE db : USE와 같음
- SHOW DATABASES : 데이터베이스 목록 조회

## 2. 테이블 정의 — CREATE TABLE

- CREATE TABLE [IF NOT EXISTS] t (컬럼, 제약, ...) : 테이블 생성
- CREATE TABLE t LIKE other : 다른 테이블의 구조를 그대로 복사해 생성
- CREATE TABLE t [(컬럼 정의)] AS SELECT ... : 쿼리 결과로 테이블 생성(데이터 포함)
- CREATE TABLE db.t (...) : 데이터베이스를 붙여서 생성
- ... ENGINE / AUTO_INCREMENT = n / [DEFAULT] CHARSET / COLLATE / COMMENT / ROW_FORMAT / KEY_BLOCK_SIZE 등 테이블 옵션 : 옵션을 적어도 받아들임(AUTO_INCREMENT 시작값 · 문자 집합 검사만 실제로 적용)
- PARTITION BY RANGE (col) (PARTITION p VALUES LESS THAN (n | MAXVALUE), ...) : 범위 파티션
- PARTITION BY LIST (col) (PARTITION p VALUES IN (...), ...) : 목록 파티션
- PARTITION BY HASH (col) PARTITIONS n : 해시 파티션

### 컬럼 정의 `이름 타입 [옵션...]`

- PRIMARY KEY : 기본 키(NOT NULL 포함)
- NOT NULL / NULL : NULL 허용 여부
- DEFAULT 값 / DEFAULT (값) / DEFAULT NULL : 기본값
- DEFAULT CURRENT_TIMESTAMP[(n)] / NOW() : 입력 시각을 기본값으로
- ON UPDATE CURRENT_TIMESTAMP[(n)] : 수정할 때 시각을 자동 갱신
- AUTO_INCREMENT : 자동 증가 번호
- UNIQUE : 중복 불가
- CHECK (조건) [[NOT] ENFORCED] : 값 검사 조건
- REFERENCES t(col) [ON DELETE 동작] [ON UPDATE 동작] : 컬럼에 바로 붙이는 외래 키
- COMMENT '설명' : 컬럼 설명
- CHARACTER SET x / CHARSET x / COLLATE x : 문자 집합 · 정렬 규칙 표기(utf8mb4 계열 · `_ci`만 받음)
- UNSIGNED / SIGNED : 부호 없는 정수 / 부호 있는 정수

### 테이블 제약 · 인덱스 (컬럼 목록 뒤에 적음)

- PRIMARY KEY (a, b, ...) : 복합 기본 키
- UNIQUE [KEY | INDEX] [이름] (a, b, ...) : 복합 포함 유일 키
- KEY | INDEX [이름] (a, b, ...) : 일반 인덱스(`USING HASH`, `COMMENT`, `VISIBLE`/`INVISIBLE`도 받음)
- CONSTRAINT 이름 PRIMARY KEY / UNIQUE / CHECK / FOREIGN KEY ... : 이름 붙인 제약
- [CONSTRAINT 이름] FOREIGN KEY (col) REFERENCES t(col) [ON DELETE 동작] [ON UPDATE 동작] : 외래 키(컬럼 하나짜리만)
- 외래 키 동작 : CASCADE / SET NULL / SET DEFAULT / RESTRICT / NO ACTION
- 자기 표를 가리키는 외래 키 : `REFERENCES emp(id)`를 `emp` 자신에게(R55) — `DELETE`는 InnoDB처럼 행을 하나씩 읽는 순서(키 순서, `ORDER BY`가 있으면 그 순서)로 처리한다: RESTRICT는 아직 있는 자식이 있으면 문장 전체를 거절하고, CASCADE는 자식을 되풀이해서 지우되 14단계까지(15번째 단계는 오류 3008), SET NULL은 자식을 NULL로 바꾼다. 같은 문장이 먼저 연쇄로 지운 행은 스캔이 도달하지 않아 세지 않는다. `ON UPDATE CASCADE`·`SET NULL`은 자기 표에는 쓸 수 없어 RESTRICT처럼 동작한다(MySQL과 같음)

## 3. 테이블 변경 · 삭제 · 이름 변경

- ALTER TABLE t ADD [COLUMN] 컬럼 정의 [FIRST | AFTER col] : 컬럼 추가
- ALTER TABLE t MODIFY [COLUMN] 컬럼 정의 : 컬럼 타입 · 제약 변경(값도 새 타입으로 바꾸고 인덱스를 다시 만듦)
- ALTER TABLE t CHANGE [COLUMN] 옛이름 새이름 타입... : 컬럼 이름과 정의를 함께 변경
- ALTER TABLE t RENAME COLUMN a TO b : 컬럼 이름 변경
- ALTER TABLE t DROP [COLUMN] col : 컬럼 삭제
- ALTER TABLE t ALTER [COLUMN] col SET DEFAULT 값 : 기본값 지정
- ALTER TABLE t ALTER [COLUMN] col DROP DEFAULT : 기본값 제거
- ALTER TABLE t ADD [UNIQUE] INDEX | KEY [이름] (cols) : 인덱스 추가
- ALTER TABLE t ADD CONSTRAINT [이름] UNIQUE (cols) : 유일 제약 추가
- ALTER TABLE t ADD CONSTRAINT [이름] CHECK (조건) : 검사 제약 추가
- ALTER TABLE t ADD [CONSTRAINT 이름] FOREIGN KEY (col) REFERENCES t2(col) [ON DELETE ..] [ON UPDATE ..] : 외래 키 추가
- ALTER TABLE t DROP INDEX | KEY 이름 : 인덱스 삭제
- ALTER TABLE t DROP CONSTRAINT 이름 : 제약 삭제
- ALTER TABLE t DROP FOREIGN KEY 이름 : 외래 키 삭제
- ALTER TABLE t RENAME [TO | AS] new : 테이블 이름 변경
- ALTER TABLE t ADD PARTITION (PARTITION p VALUES ...) : 파티션 추가
- ALTER TABLE t DROP PARTITION p : 파티션 삭제
- ALTER TABLE t ENGINE = x / AUTO_INCREMENT = n / [DEFAULT] CHARSET / COLLATE 등 테이블 옵션 : 옵션 변경
- ALTER TABLE t DISABLE | ENABLE KEYS : 받아들이고 아무것도 안 함(MySQL 호환)
- ALTER TABLE t 동작1, 동작2, ... : 여러 동작을 한 문장으로(전부 되거나 전부 안 됨)
- ALTER VIEW v AS SELECT ... : 뷰 다시 정의
- DROP TABLE [IF EXISTS] t1[, t2, ...] : 테이블 삭제(다른 테이블의 외래 키가 가리키면 거부)
- TRUNCATE [TABLE] t : 테이블의 모든 행 삭제(자동 증가 번호도 처음으로)
- RENAME TABLE a TO b[, c TO d, ...] : 테이블 이름 변경(여러 개 한 번에)

## 4. 인덱스 · 뷰 · 별칭

- CREATE INDEX 이름 ON t (a [ASC|DESC], b, ...) : 단일 · 복합 인덱스 생성
- CREATE UNIQUE INDEX 이름 ON t (cols) : 유일 인덱스 생성
- CREATE INDEX 이름 USING HASH | BTREE ON t (cols) : 인덱스 종류 지정(HASH는 해시 인덱스, 그 밖은 B+Tree)
- DROP INDEX [IF EXISTS] 이름 [ON t] : 인덱스 삭제
- CREATE [OR REPLACE] [ALGORITHM = x] [DEFINER = x] [SQL SECURITY x] VIEW v AS SELECT ... : 뷰 생성 · 덮어쓰기
- DROP VIEW [IF EXISTS] v : 뷰 삭제
- INSERT / UPDATE / DELETE 대상으로 뷰 사용 : 단순 뷰(JOIN · DISTINCT · GROUP BY 없음)는 쓰기도 가능
- CREATE SYNONYM 이름 FOR 대상 : 테이블 별칭 생성
- CREATE OR REPLACE SYNONYM 이름 FOR 대상 : 별칭 덮어쓰기
- DROP SYNONYM [IF EXISTS] 이름 : 별칭 삭제
- SHOW SYNONYMS : 별칭 목록 조회

## 5. 데이터 입력 · 수정 · 삭제 (DML)

### INSERT / REPLACE

- INSERT INTO t [(cols)] VALUES (...)[, (...)...] : 행 입력(여러 행 한 번에, 값 자리에 식 · DEFAULT · 변수 가능)
- INSERT INTO t SET col = 값, ... : SET 형식으로 입력
- INSERT INTO t [(cols)] SELECT ... : 쿼리 결과 입력(UNION 포함)
- INSERT IGNORE INTO ... : 제약에 걸린 행은 조용히 건너뜀
- INSERT ... ON DUPLICATE KEY UPDATE col = 값, ... : 키가 겹치면 UPDATE로 전환(`VALUES(col)` 사용 가능)
- INSERT ... RETURNING cols | * : 입력한 행을 바로 돌려줌
- REPLACE [INTO] t [(cols)] VALUES (...) : 키가 겹치면 지우고 다시 입력
- REPLACE INTO t SET ... / REPLACE INTO t SELECT ... : REPLACE의 SET · SELECT 형식

### UPDATE

- UPDATE t SET col = 식[, ...] [WHERE ...] : 행 수정(식 · 함수 · 서브쿼리 · DEFAULT 가능)
- UPDATE t SET ... ORDER BY ... LIMIT n : 정렬 순서대로 n행만 수정
- UPDATE t SET ... RETURNING cols : 수정한 행을 돌려줌
- UPDATE t1 JOIN t2 ON ... SET ... : JOIN으로 여러 테이블 수정
- UPDATE t1, t2 SET ... WHERE ... : 쉼표 형식 다중 테이블 수정
- UPDATE t 별칭 SET 별칭.col = ... : 별칭 사용(같은 테이블을 두 번 쓰는 자체 JOIN 포함)

### DELETE

- DELETE FROM t [WHERE ...] : 행 삭제
- DELETE FROM t ... ORDER BY ... LIMIT n : 정렬 순서대로 n행만 삭제
- DELETE FROM t ... RETURNING cols : 삭제한 행을 돌려줌
- DELETE t1[, t2] FROM t1 JOIN t2 ON ... WHERE ... : JOIN으로 여러 테이블에서 삭제

### MERGE

- MERGE INTO 대상 [별칭] USING 원본 [별칭] ON 조건 : 원본과 대상을 맞춰 보는 병합
- WHEN MATCHED [AND 조건] THEN UPDATE SET ... : 일치할 때 수정
- WHEN MATCHED [AND 조건] THEN DELETE : 일치할 때 삭제
- WHEN NOT MATCHED THEN INSERT (cols) VALUES (...) : 일치하지 않을 때 입력

## 6. 조회 — SELECT

### 기본

- SELECT 식, ... : 값 · 식 계산(FROM 없이도 가능)
- SELECT * / t.* : 모든 컬럼
- SELECT 식 [AS] 별칭 : 결과 컬럼에 이름 붙이기
- SELECT DISTINCT ... : 중복 행 제거
- SELECT ... FROM t [[AS] 별칭] : 테이블(별칭 포함)에서 조회
- SELECT ... FROM db.t : 데이터베이스를 붙여 조회
- SELECT ... FROM (SELECT ...) 별칭 : 파생 테이블(FROM 절 서브쿼리)
- SELECT ... INTO @변수[, @변수] FROM ... : 결과를 변수에 담기
- SELECT SQL_CALC_FOUND_ROWS ... : LIMIT 앞의 전체 행 수를 FOUND_ROWS()로 읽기 위해 계산
- SELECT /*+ MAX_EXECUTION_TIME(ms) */ ... : 실행 시간 제한 힌트(다른 힌트는 주석으로 무시)

### 조건 · 정렬 · 제한 · 그룹

- WHERE 조건 : 행 거르기
- 조건 연산 : `AND` `OR` `NOT` `XOR` `=` `<>` `!=` `<` `<=` `>` `>=` `<=>` — 비교 · 논리 연결
- [NOT] IN (값, ... | 서브쿼리) : 목록 · 서브쿼리에 포함 여부
- [NOT] BETWEEN a AND b : 범위 안 여부
- [NOT] LIKE 'pattern' [ESCAPE 'c'] : 문자열 패턴(`%` `_`) 일치 여부(`\%` · `\_`는 그 글자 자체, ESCAPE로 다른 이스케이프 문자를 지정, 패턴은 열 · 식에서 읽어도 됨)
- [NOT] REGEXP / RLIKE 'pattern' : 정규식 일치 여부
- IS [NOT] NULL / IS [NOT] TRUE / IS [NOT] FALSE / IS [NOT] UNKNOWN : NULL · 참 · 거짓 · 알 수 없음 검사
- [NOT] EXISTS (서브쿼리) : 서브쿼리 결과 존재 여부
- 비교 연산자 ANY / SOME / ALL (서브쿼리) : 서브쿼리 값 중 하나 / 전부와 비교
- (a, b) IN ((1, 2), ...) / (a, b) = (1, 2) : 행 생성자 비교
- GROUP BY 컬럼 | 별칭 | 위치번호 | 식 : 그룹으로 묶기
- HAVING 조건 : 그룹을 거르기(select 별칭 · 집계 · 서브쿼리 사용 가능)
- ORDER BY 컬럼 | 별칭 | 위치번호 | 식 [ASC | DESC], ... : 정렬
- LIMIT n / LIMIT n OFFSET m / LIMIT m, n : 행 수 제한 · 건너뛰기
- FETCH FIRST | NEXT n ROWS ONLY : 표준 SQL 행 수 제한

### 조인

- [INNER] JOIN t ON 조건 : 두 테이블의 일치하는 행 결합
- JOIN t USING (col, ...) : 같은 이름의 컬럼으로 결합
- LEFT [OUTER] JOIN : 왼쪽 행은 모두 유지
- RIGHT [OUTER] JOIN : 오른쪽 행은 모두 유지
- FULL [OUTER] JOIN : 양쪽 행을 모두 유지
- CROSS JOIN : 모든 조합(카테시안 곱)
- NATURAL JOIN : 이름이 같은 컬럼으로 자동 결합
- FROM a, b WHERE ... : 쉼표 조인
- JOIN LATERAL (서브쿼리) 별칭 ON ... : 앞 테이블의 컬럼을 쓰는 서브쿼리와 결합
- JOIN (서브쿼리) 별칭 ON ... : 파생 테이블과 결합
- 같은 테이블을 별칭으로 여러 번 조인 : 자체 조인

### 서브쿼리 · 집합 · CTE

- (SELECT ...) : 스칼라 서브쿼리(select 목록 · WHERE · HAVING · 식 안, 바깥 쿼리 컬럼 참조 가능)
- SELECT ... UNION [ALL] SELECT ... : 결과 합치기(ALL은 중복 유지)
- SELECT ... INTERSECT [ALL] SELECT ... : 두 결과의 교집합
- SELECT ... EXCEPT [ALL] SELECT ... : 첫 결과에서 두 번째 결과를 뺀 차집합
- UNION 뒤 ORDER BY / LIMIT : 합친 결과 정렬 · 제한
- WITH name [(컬럼, ...)] AS (SELECT ...)[, ...] SELECT ... : 공통 테이블 식(CTE)
- WITH RECURSIVE name AS (기준 UNION [ALL] 재귀) SELECT ... : 재귀 CTE
- WITH ... INSERT / UPDATE / DELETE ... : CTE를 쓰는 쓰기 문장(CTE 이름을 서브쿼리에서 읽음)

### 잠금 읽기

- SELECT ... FOR UPDATE : 읽은 행을 쓰기 잠금
- SELECT ... FOR SHARE / LOCK IN SHARE MODE : 읽은 행을 공유 잠금
- FOR UPDATE | FOR SHARE NOWAIT : 잠겨 있으면 기다리지 않고 바로 오류
- FOR UPDATE | FOR SHARE SKIP LOCKED : 잠긴 행은 건너뛰고 읽음

### 윈도우 함수 구문

- 함수() OVER ([PARTITION BY ...] [ORDER BY ...] [프레임]) : 행 집합 위에서 계산
- 함수() OVER 이름 + WINDOW 이름 AS (...) : 이름 붙인 윈도우 정의
- ROWS | RANGE BETWEEN 시작 AND 끝 : 프레임 범위(`UNBOUNDED PRECEDING` · `n PRECEDING` · `CURRENT ROW` · `n FOLLOWING` · `UNBOUNDED FOLLOWING`)
- ROWS | RANGE 시작 : 프레임 짧은 형식(`ROWS UNBOUNDED PRECEDING` 등)
- 집계 함수 FILTER (WHERE 조건) : 조건에 맞는 행만 집계

## 7. 표현식 · 연산자 · 리터럴

- 산술 : `+` `-` `*` `/` `%` `DIV` `MOD(a, b)` — 사칙 · 나머지 · 정수 나눗셈
- 비트 : `&` `|` `^` `~` `<<` `>>` — 비트 연산
- 논리 : `AND` `OR` `NOT` `XOR` `||` — 논리 연산(`||`는 OR이고, sql_mode에 `PIPES_AS_CONCAT`이면 문자열 연결; `&&`는 지원하지 않음)
- 비교 : `=` `<>` `!=` `<` `<=` `>` `>=` `<=>`(NULL 안전 같음) — 비교(값으로도 사용 가능)
- CASE WHEN 조건 THEN 값 ... [ELSE 값] END : 조건 분기
- CASE 식 WHEN 값 THEN 값 ... END : 값 비교 분기
- IF(조건, 참값, 거짓값) : 삼항 선택
- CAST(식 AS 타입) : 타입 변환(SIGNED · UNSIGNED · CHAR · DATE · DECIMAL(m,n) · BINARY 등)
- CONVERT(식, 타입) : CAST와 같은 타입 변환
- 식 + INTERVAL n 단위 / 식 - INTERVAL n 단위 : 날짜 더하기 · 빼기(YEAR · MONTH · DAY · HOUR · MINUTE · SECOND ...)
- EXTRACT(단위 FROM 날짜) : 날짜에서 년 · 월 · 일 등 뽑기
- TRIM([LEADING | TRAILING | BOTH] [문자] FROM 문자열) : 양쪽 / 앞 / 뒤 문자 제거
- SUBSTRING(문자열 FROM n [FOR m]) : 문자열 일부(쉼표 형식도 가능)
- POSITION(부분 IN 문자열) : 위치 찾기
- 식 -> '$.경로' / 식 ->> '$.경로' : JSON 값 꺼내기 / 따옴표 없는 문자열로 꺼내기
- 'text' / "text" : 문자열 리터럴(`''` 따옴표 두 번, `\n` `\t` `\\` `\'` 이스케이프, `\%` · `\_`는 두 글자 그대로 남아 LIKE가 읽음; 큰따옴표는 컬럼 이름으로 읽음)
- 123 / 1.5 / 1e3 / 0x41 : 숫자 · 지수 · 16진수 리터럴
- TRUE / FALSE / NULL : 참 · 거짓 · 값 없음
- @변수 / @@서버변수 / @@global.변수 / @@session.변수 : 사용자 변수 · 서버 변수 읽기
- ( 식 ) / (SELECT ...) : 괄호 · 서브쿼리 식
- 함수 호출 · 사용자 정의 함수 호출 : 식 어디에서나 사용(9장)

## 8. 데이터 타입

- TINYINT / SMALLINT / MEDIUMINT / INT / INTEGER / BIGINT [UNSIGNED] : 정수(INT2 · INT3 · INT4 · INT8 별칭 포함, `(n)` 표시 폭은 무시)
- BOOL / BOOLEAN : 참 · 거짓(TINYINT(1))
- FLOAT[(m,d)] / DOUBLE[(m,d)] / REAL / DOUBLE PRECISION : 부동소수점 실수
- DECIMAL(m,d) / NUMERIC / DEC / FIXED : 정확한 소수
- CHAR(n) / VARCHAR(n) / NCHAR / NVARCHAR / CHARACTER [VARYING](n) : 문자열
- TEXT / TINYTEXT / MEDIUMTEXT / LONGTEXT / CLOB : 긴 문자열
- BLOB / TINYBLOB / MEDIUMBLOB / LONGBLOB : 바이너리 덩어리(문자열처럼 저장)
- DATE : 날짜
- TIME[(n)] : 시간(소수 초 n자리, 최대 6)
- DATETIME[(n)] / TIMESTAMP[(n)] : 날짜 + 시간
- YEAR : 연도
- ENUM('a', 'b', ...) : 목록 중 하나
- SET('a', 'b', ...) : 목록 중 여러 개
- JSON : JSON 문서

## 9. 함수

### 집계 함수

- COUNT(*) / COUNT(col) / COUNT(DISTINCT a[, b]) : 행 수
- SUM([DISTINCT] col) : 합계
- AVG([DISTINCT] col) : 평균
- MIN(col) / MAX(col) : 최솟값 / 최댓값
- GROUP_CONCAT([DISTINCT] col [ORDER BY ...] [SEPARATOR '구분']) : 값을 이어 붙인 문자열
- STDDEV / STD / STDDEV_POP / STDDEV_SAMP : 표준편차(모집단 / 표본)
- VARIANCE / VAR_POP / VAR_SAMP : 분산(모집단 / 표본)
- BIT_AND / BIT_OR : 비트 AND / OR 집계
- MEDIAN : 중앙값
- JSON_AGG / ARRAY_AGG : 값을 JSON 배열로 모음
- ANY_VALUE(col) : 그룹에서 아무 값 하나(ONLY_FULL_GROUP_BY 회피용)

### 윈도우 함수 (`OVER` 필요)

- ROW_NUMBER() : 행 번호
- RANK() / DENSE_RANK() : 순위(동률 뒤 건너뜀 / 건너뛰지 않음)
- PERCENT_RANK() / CUME_DIST() : 백분율 순위 / 누적 분포
- NTILE(n) : n개 구간으로 나누기
- LAG(col[, n[, 기본값]]) / LEAD(col[, n[, 기본값]]) : 앞 / 뒤 행의 값
- FIRST_VALUE(col) / LAST_VALUE(col) / NTH_VALUE(col, n) : 프레임의 첫 / 마지막 / n번째 값
- SUM · AVG · COUNT · MIN · MAX ... OVER (...) : 집계를 윈도우 위에서(누적 합계 · 이동 평균 등)

### 문자열 함수

- CONCAT / CONCAT_WS : 문자열 잇기(구분자 포함)
- LENGTH / OCTET_LENGTH / BIT_LENGTH : 바이트 · 비트 길이
- CHAR_LENGTH / CHARACTER_LENGTH : 글자 수
- UPPER / UCASE / LOWER / LCASE : 대문자 / 소문자 변환
- TRIM / LTRIM / RTRIM : 공백(또는 지정 문자) 제거
- LEFT / RIGHT / SUBSTR / SUBSTRING / MID / SUBSTRING_INDEX : 문자열 자르기
- LPAD / RPAD : 지정 길이까지 채우기
- REPLACE / INSERT / REVERSE / REPEAT / SPACE : 바꾸기 · 끼워넣기 · 뒤집기 · 반복 · 공백 만들기
- INSTR / LOCATE / FIELD / FIND_IN_SET / ELT : 위치 찾기 · 목록에서 고르기
- ASCII / ORD / CHAR : 문자 코드 ↔ 문자
- STRCMP : 문자열 비교(-1 / 0 / 1)
- FORMAT : 천 단위 쉼표와 소수 자릿수로 표시
- HEX / UNHEX / BIN / OCT / CONV : 16 · 2 · 8진수와 진법 변환
- TO_BASE64 / FROM_BASE64 : Base64 변환
- QUOTE / SOUNDEX / MAKE_SET / EXPORT_SET : 따옴표 감싸기 · 발음 코드 · 집합 문자열
- MD5 / SHA / SHA1 / SHA2 / CRC32 : 해시 · 체크섬
- UUID : 고유 식별자 생성

### 정규식 함수

- REGEXP_LIKE : 정규식 일치 여부
- REGEXP_REPLACE : 정규식으로 바꾸기
- REGEXP_SUBSTR : 정규식에 맞는 부분 꺼내기
- REGEXP_INSTR : 정규식에 맞는 위치 찾기

### 숫자 함수

- ABS / SIGN : 절댓값 / 부호
- CEIL / CEILING / FLOOR / ROUND / TRUNCATE : 올림 · 내림 · 반올림 · 버림
- MOD / POW / POWER / SQRT / EXP : 나머지 · 거듭제곱 · 제곱근 · 지수
- LN / LOG / LOG2 / LOG10 : 로그
- SIN / COS / TAN / COT / ASIN / ACOS / ATAN / ATAN2 : 삼각 함수
- DEGREES / RADIANS / PI : 각도 변환 · 원주율
- GREATEST / LEAST : 여러 값 중 최대 / 최소
- RAND : 난수
- BIT_COUNT : 켜진 비트 수

### 날짜 · 시간 함수

- NOW / CURRENT_TIMESTAMP / LOCALTIME / LOCALTIMESTAMP / SYSDATE : 지금 날짜 · 시간
- CURDATE / CURRENT_DATE / CURTIME / CURRENT_TIME : 오늘 날짜 / 지금 시간
- UTC_DATE / UTC_TIME / UTC_TIMESTAMP : UTC 기준 현재
- DATE / TIME / TIMESTAMP : 값에서 날짜 부분 / 시간 부분 / 날짜시간 만들기
- YEAR / MONTH / DAY / DAYOFMONTH / HOUR / MINUTE / SECOND / MICROSECOND / QUARTER : 부분 뽑기
- DAYOFWEEK / WEEKDAY / DAYOFYEAR / WEEK / WEEKOFYEAR / YEARWEEK : 요일 · 연중 일 · 주 번호
- MONTHNAME / DAYNAME : 월 · 요일 이름
- LAST_DAY : 그 달의 마지막 날
- DATE_ADD / ADDDATE / DATE_SUB / SUBDATE : 날짜에 간격 더하기 · 빼기
- ADDTIME / SUBTIME : 시간 더하기 · 빼기
- DATEDIFF / TIMEDIFF / TIMESTAMPDIFF / TIMESTAMPADD : 차이 · 더하기
- PERIOD_ADD / PERIOD_DIFF : 연월(YYYYMM) 더하기 · 차이
- DATE_FORMAT / TIME_FORMAT / GET_FORMAT : 서식 문자열로 출력
- STR_TO_DATE : 서식에 맞는 문자열을 날짜로
- TO_DAYS / FROM_DAYS / TO_SECONDS : 일 · 초 수 변환
- MAKEDATE / MAKETIME : 년+일수 / 시+분+초로 만들기
- SEC_TO_TIME / TIME_TO_SEC : 초 ↔ 시간
- UNIX_TIMESTAMP / FROM_UNIXTIME : 유닉스 시각 변환

### 조건 · NULL 함수

- COALESCE : 첫 번째 NULL 아닌 값
- IFNULL / ISNULL / NULLIF : NULL 대체 · 검사 · 같으면 NULL
- IF : 조건 선택

### JSON 함수

- JSON_VALID / JSON_TYPE / JSON_LENGTH / JSON_KEYS : JSON 검사 · 종류 · 길이 · 키 목록
- JSON_ARRAY / JSON_OBJECT : JSON 배열 · 객체 만들기
- JSON_EXTRACT / JSON_VALUE / JSON_UNQUOTE : 경로로 값 꺼내기 · 따옴표 벗기기
- JSON_CONTAINS : 포함 여부
- JSON_SET / JSON_INSERT / JSON_REPLACE / JSON_REMOVE : 경로의 값을 넣기 · 새로 넣기 · 바꾸기 · 지우기

### 시스템 · 세션 함수

- DATABASE / SCHEMA : 현재 데이터베이스
- USER / CURRENT_USER / SESSION_USER / SYSTEM_USER : 접속한 사용자
- VERSION : 서버 버전
- LAST_INSERT_ID : 마지막 AUTO_INCREMENT 값
- ROW_COUNT : 마지막 문장이 바꾼 행 수
- FOUND_ROWS : LIMIT 앞의 전체 행 수(SQL_CALC_FOUND_ROWS와 함께)
- SLEEP : 지정한 초만큼 대기
- 사용자 정의 함수(CREATE FUNCTION) : 직접 만든 함수 호출

## 10. 트랜잭션 · 잠금 · 세션 설정

### 트랜잭션

- BEGIN [WORK] / START TRANSACTION [옵션] : 트랜잭션 시작
- COMMIT [WORK] : 변경 확정
- ROLLBACK [WORK] : 변경 취소
- SAVEPOINT 이름 : 저장점 지정
- ROLLBACK TO [SAVEPOINT] 이름 : 저장점까지만 되돌리기
- RELEASE SAVEPOINT 이름 : 저장점 해제
- SET autocommit = 0 | 1 : 자동 커밋 켜고 끄기
- SET [SESSION] TRANSACTION ISOLATION LEVEL 수준 : 격리 수준 지정(READ UNCOMMITTED · READ COMMITTED · REPEATABLE READ · SERIALIZABLE)
- SET ISOLATION LEVEL 수준 : 위와 같음(짧은 형식)
- SHOW ISOLATION LEVEL : 현재 격리 수준 조회

### 테이블 잠금

- LOCK TABLES t READ | WRITE[, t2 READ | WRITE ...] : 테이블 잠금
- UNLOCK TABLES : 테이블 잠금 해제
- SHOW LOCKS : 현재 잠금 목록

### 변수 · SET

- SET @변수 = 식 : 사용자 변수 지정(서브쿼리도 가능)
- SET @a = 1, @b = 2 : 여러 변수 한 번에
- SET [GLOBAL | SESSION | PERSIST] 변수 = 값 : 서버 변수 지정(없는 변수는 저장만 하고 `@@변수`로 읽힘)
- SET @@[global. | session.]변수 = 값 : 서버 변수를 `@@`로 지정
- SET NAMES 문자집합 [COLLATE 규칙] / SET CHARACTER SET 문자집합 : 접속 문자 집합 지정(받아들이기만 함)
- SET FOREIGN_KEY_CHECKS = 0 | 1 : 외래 키 검사 켜고 끄기(덤프 복원용)
- SET max_execution_time = ms : 문장 실행 시간 제한
- SET sql_mode = '...' : SQL 모드 지정(PIPES_AS_CONCAT · ANSI 등)

### 준비된 문장

- PREPARE 이름 FROM 'SQL ... ? ...' : 문장을 준비(`?` 자리표시자)
- EXECUTE 이름 [USING @a, @b, ...] : 준비한 문장 실행
- DEALLOCATE PREPARE 이름 : 준비한 문장 해제

## 11. 조회 · 관리 · 모니터링

### 구조 조회

- SHOW TABLES [FROM db] [LIKE 'pattern'] : 테이블 목록
- DESCRIBE t : 테이블 구조
- SHOW CREATE TABLE t : 테이블을 만드는 CREATE 문
- SHOW CREATE VIEW v : 뷰를 만드는 CREATE 문
- SHOW INDEX FROM t : 인덱스 목록
- SHOW GRANTS [FOR 'user'@'host'] : 권한 목록
- SHOW ROLES : 역할 목록

### 실행 · 성능

- EXPLAIN SELECT ... : 실행 계획(접근 경로 · 조인 알고리즘 · 비용; SELECT가 아닌 문장은 계획 없이 안내만)
- EXPLAIN ANALYZE 문장 : 실제로 실행하며 단계별 행 수 · 시간 · 비율(조인 종류 · 알고리즘, 서브쿼리 시간 포함; 쓰기 문장은 실제로 바뀜)
- TRACE 문장 : 단계별 시간 · 잠금 대기 · 인덱스 사용을 추적하며 실행
- ANALYZE TABLE t : 통계 정보 갱신
- VACUUM [t] : 지운 행의 옛 버전 정리
- CHECKPOINT : redo 로그를 테이블 파일에 반영하고 비움

### 서버 상태 · 세션

- SHOW STATUS / SHOW SESSION STATUS [LIKE 'pattern'] : 이 세션의 상태 카운터(문장 종류별 개수 · Questions)
- SHOW GLOBAL STATUS [LIKE 'pattern'] : 서버 전체 카운터(Com_* 문장 수 · Connections · Threads_* · Uptime · Innodb_* 버퍼 풀 / redo / 행 잠금 대기)
- SHOW PROCESSLIST : 접속 중인 세션과 실행 중인 문장
- KILL [QUERY] n : 세션 n이 실행 중인 문장을 중단(SELECT는 다음 행에서, 잠금 대기는 즉시; 쓰기 도중엔 끝까지)
- KILL CONNECTION n / KILL n : 세션 n의 접속을 끊고 그 세션의 트랜잭션을 되돌림
- SHOW BUFFER POOL : 버퍼 풀 상태
- SHOW WAL : redo 로그 상태

### information_schema (SELECT로 조회)

- information_schema.tables / columns / statistics : 테이블 · 컬럼 · 인덱스 정보
- information_schema.schemata (schemas) : 데이터베이스 목록
- information_schema.key_column_usage / table_constraints / referential_constraints : 키 · 제약 · 외래 키 정보
- information_schema.views : 뷰 정보
- information_schema.character_sets / collations / engines : 문자 집합 · 정렬 규칙 · 엔진 목록
- information_schema.processlist : 세션 목록
- information_schema.innodb_trx / innodb_locks / innodb_lock_waits / innodb_deadlocks : 트랜잭션 · 잠금 · 잠금 대기 · 교착 상태
- information_schema.row_versions : 행의 버전(MVCC) 정보
- information_schema.recovery : 마지막 시작 때의 복구 기록
- information_schema.global_status : SHOW GLOBAL STATUS와 같은 값을 테이블로

### 백업 · 복원

- BACKUP [DATABASE db] [INTO 'file'] : 데이터베이스를 SQL 덤프로 백업
- RESTORE FROM 'file' [DATABASE db] : 백업 파일에서 복원(다른 이름의 DB로도 가능)

## 12. 사용자 · 권한 · 역할

- CREATE USER [IF NOT EXISTS] 'u'@'host' [IDENTIFIED BY 'pw'] : 사용자 생성
- DROP USER [IF EXISTS] 'u'@'host' : 사용자 삭제
- GRANT 권한, ... ON [TABLE | DATABASE] db.t | db.* | *.* TO 'u'@'host' [WITH GRANT OPTION] : 권한 부여
- REVOKE 권한, ... ON ... FROM 'u'@'host' : 권한 회수
- 권한 이름 : SELECT · INSERT · UPDATE · DELETE · CREATE · DROP · ALTER · INDEX · ALL [PRIVILEGES] · GRANT OPTION
- CREATE ROLE 이름 : 역할 생성
- DROP ROLE [IF EXISTS] 이름 : 역할 삭제
- GRANT ROLE 역할 TO 'u'@'host' [WITH ADMIN OPTION] : 사용자에게 역할 부여
- REVOKE ROLE 역할 FROM 'u'@'host' : 역할 회수
- GRANT 권한 ON ... TO 역할 : 역할에 권한 부여

## 13. 저장 프로시저 · 함수 · 트리거

- CREATE PROCEDURE 이름([IN | OUT | INOUT] 매개변수 타입, ...) BEGIN ... END : 저장 프로시저 생성(한 문장 본문도 가능; `DEFINER = x`, `DETERMINISTIC`, `COMMENT`, `SQL SECURITY` 표기 가능)
- CALL 이름([인수, ...]) : 프로시저 실행(식 · @변수 인수, OUT/INOUT은 @변수로 돌려받음)
- DROP PROCEDURE [IF EXISTS] 이름 : 프로시저 삭제
- CREATE FUNCTION 이름(매개변수 타입, ...) RETURNS 타입 [DETERMINISTIC] RETURN 식 : 사용자 정의 함수 생성
- DROP FUNCTION [IF EXISTS] 이름 : 함수 삭제
- CREATE TRIGGER 이름 BEFORE | AFTER INSERT | UPDATE | DELETE ON t FOR EACH ROW 본문 : 행마다 도는 트리거 생성(`NEW.col` · `OLD.col` 사용)
- DROP TRIGGER [IF EXISTS] 이름 : 트리거 삭제
- 본문 안의 문장 : 일반 DML · SELECT · `SELECT ... INTO 변수` · `CALL`

프로시저 · 트리거 본문의 제어문:

- DECLARE 변수 타입 [DEFAULT 값] : 지역 변수 선언
- SET 변수 = 식 : 변수에 값 넣기(`SET NEW.col = 식`으로 BEFORE 트리거에서 입력값 바꾸기)
- IF 조건 THEN ... [ELSEIF 조건 THEN ...] [ELSE ...] END IF : 조건 분기
- WHILE 조건 DO ... END WHILE : 조건이 참인 동안 반복
- LOOP ... END LOOP : 무한 반복(LEAVE로 탈출)
- REPEAT ... UNTIL 조건 END REPEAT : 한 번 실행하고 조건이 참이 될 때까지 반복
- LEAVE 라벨 : 반복문 빠져나가기
- ITERATE 라벨 : 반복문의 다음 회차로
- 라벨: LOOP / WHILE / REPEAT : 반복문에 이름 붙이기
- RETURN 식 : 함수의 반환값(함수 본문은 `RETURN 식` 한 문장)

## 14. MySQL 프로토콜에서만 되는 문법

mysql 클라이언트 · 드라이버 · 도구가 보내는 문장을 받아주는 호환 계층입니다(네이티브 프로토콜에서는 같은 이름의 문법이 없습니다).

- SHOW [FULL] TABLES [FROM db] [LIKE '...'] : 테이블 목록(FULL은 Table_type 열 포함)
- SHOW [FULL] COLUMNS | FIELDS FROM t [FROM db] [LIKE '...'] : 컬럼 목록
- DESC t / DESCRIBE t [col] / EXPLAIN t : 컬럼 목록(MySQL의 모양)
- SHOW INDEX | INDEXES | KEYS FROM t [FROM db] : 인덱스 목록(MySQL의 모양)
- SHOW CREATE TABLE t / SHOW CREATE VIEW v : MySQL 형식의 CREATE 문
- SHOW TABLE STATUS [FROM db] [LIKE '...'] : 테이블 상태
- SHOW VARIABLES [LIKE '...'] / SHOW SESSION | GLOBAL VARIABLES : 서버 변수 목록
- SHOW CHARACTER SET / SHOW CHARSET / SHOW COLLATION : 문자 집합 · 정렬 규칙 목록
- SHOW ENGINES / SHOW PLUGINS : 엔진 · 플러그인 목록
- SHOW WARNINGS : 경고 목록(항상 비어 있음)
- SHOW EVENTS / SHOW TRIGGERS / SHOW PROCEDURE STATUS / SHOW FUNCTION STATUS : 빈 목록으로 응답
- ANALYZE | CHECK | OPTIMIZE TABLE t : 통계 갱신 · 점검 · 정리(MySQL 응답 모양)
- SELECT VERSION() / USER() / CURRENT_USER() / DATABASE() / @@변수 : 클라이언트가 접속 직후 보내는 질의
- SET NAMES / SET character_set_* / SET autocommit ... : 클라이언트의 접속 설정(모르는 변수는 OK로 응답)
- /*!40101 SET ... */ / /*M!... */ : mysqldump가 쓰는 실행 가능한 주석(실행함)
- COM_QUERY · COM_INIT_DB · COM_PING · COM_CHANGE_USER · COM_RESET_CONNECTION · COM_STMT_PREPARE / EXECUTE / CLOSE / RESET · COM_QUIT : 지원하는 프로토콜 명령

## 15. 주석 · 문장 형식

- -- 주석 / # 주석 : 줄 끝까지 주석
- /* 주석 */ : 블록 주석
- /*! 코드 */ : MySQL 8.4 버전이면 실행되는 주석(덤프 호환)
- /*+ MAX_EXECUTION_TIME(n) */ : SELECT 바로 뒤의 실행 시간 힌트
- 문장 구분 `;` : 한 번에 여러 문장을 보낼 때(앱 · 덤프 복원)는 `;`로 구분하고, 프로시저 · 트리거의 `BEGIN ... END` 안의 `;`는 구분자로 보지 않음

---

## 16. 지원하지 않는 문법

받아들이지 않고 오류를 돌려주는 문법입니다(MySQL에는 있음). 새 문법은 필요할 때 다음 라운드에서 하나씩 추가합니다.

- **테이블 정의** : `GENERATED ALWAYS AS (...)` 생성 컬럼, `DEFAULT (식)`, `CREATE TEMPORARY TABLE`, `FULLTEXT` 인덱스, 인덱스 접두 길이 `col(n)`, 여러 컬럼 외래 키, 표현식 인덱스, `ZEROFILL`, `SERIAL` · `BINARY` · `VARBINARY` · `BIT` · `GEOMETRY` 타입, `ALTER TABLE ADD/DROP PRIMARY KEY`, `ALTER TABLE CONVERT TO CHARACTER SET`, 파티션 `KEY` · `COLUMNS` · `REORGANIZE` · `TRUNCATE PARTITION`, `DROP TABLE ... CASCADE`, `CREATE INDEX IF NOT EXISTS`, `ALTER DATABASE`
- **뷰** : `CREATE VIEW v (컬럼 목록) AS ...`, `WITH CHECK OPTION`
- **데이터 쓰기** : `INSERT ... VALUES ()`(빈 값), `INSERT ... DEFAULT VALUES`, `INSERT ... AS 별칭`, `INSERT/UPDATE/DELETE LOW_PRIORITY · QUICK`, `UPDATE IGNORE`, `UPDATE ... JOIN (파생 테이블)`, `DELETE FROM ... USING`, `LOAD DATA`, `SELECT ... INTO OUTFILE`
- **조회** : `GROUP BY ... WITH ROLLUP`, `TABLE t`, `VALUES ROW(...)`, `USE | FORCE | IGNORE INDEX` 힌트, `PARTITION (p)` 선택, `STRAIGHT_JOIN`, `SELECT ALL | HIGH_PRIORITY`, `FROM DUAL`, `NULLS FIRST | LAST`, `OFFSET n ROWS`, `FOR UPDATE OF 테이블`, 괄호로 감싼 `(SELECT ..) UNION (SELECT ..) ORDER BY`, `NATURAL LEFT JOIN`, `IN (SELECT ... UNION SELECT ...)`, `INSERT ... SELECT ... ON DUPLICATE KEY UPDATE`
- **식 · 리터럴** : `IS [NOT] DISTINCT FROM`, `BETWEEN SYMMETRIC`, `.5`, `b'101'`, `DATE '2024-01-01'`, `_utf8mb4'x'`, `COLLATE` 연산자, `BINARY` 연산자, `MATCH ... AGAINST`, `SOUNDS LIKE`, `SELECT @a := 식`, `CONVERT(식 USING 문자집합)`, `DEFAULT(col)`, `GROUPING()`
- **함수** : `PERCENTILE_CONT ... WITHIN GROUP`, `BIT_XOR`, `CONNECTION_ID`, `GET_LOCK`, `INET_ATON`, `BENCHMARK`, `JSON_MERGE_PATCH` 등 위 9장에 없는 함수
- **프로시저** : `SIGNAL`, `DECLARE ... HANDLER`, 커서(`DECLARE CURSOR`), 본문의 `CASE ... END CASE`, 반복문 끝의 라벨(`END WHILE lp`), `CREATE PROCEDURE IF NOT EXISTS`, `CREATE OR REPLACE PROCEDURE`, `CREATE TRIGGER IF NOT EXISTS`, 여러 문장 함수 본문(`BEGIN ... RETURN ... END`)
- **권한 · 사용자** : `ALTER USER`, 컬럼 단위 `GRANT SELECT (col)`, 역할을 `GRANT 역할 TO 사용자` 형식으로 부여
- **조회 · 관리** : `SHOW CREATE DATABASE`, `SHOW CREATE PROCEDURE`, `SHOW CREATE TRIGGER`, `SHOW TABLES IN db`, `SHOW FULL PROCESSLIST`, `SHOW ENGINE INNODB STATUS`, `SHOW MASTER STATUS`, `SHOW SLAVE STATUS`, `SHOW PRIVILEGES`, `SHOW ERRORS`, `SHOW OPEN TABLES`, `SHOW DATABASES LIKE`, `EXPLAIN FORMAT=JSON`, `EXPLAIN EXTENDED`, `EXPLAIN INSERT | UPDATE | DELETE`의 실행 계획(안내만 나옴), `information_schema.routines`, `information_schema.triggers`, `FLUSH`, `HELP`, `DELIMITER`, `XA`, `OPTIMIZE TABLE` · `CHECK TABLE`(네이티브 프로토콜), `ROLLBACK AND CHAIN`, `LOCK TABLES t AS 별칭`, `DROP PREPARE`
- **복제 · 분산** : 복제(replication), XA 분산 트랜잭션, 실제 페이지 단위 버퍼 풀, 이벤트 스케줄러(`CREATE EVENT`)

> 이 문서는 RuSQL 엔진이 받아들이는 문법을 실제로 실행해 확인한 목록입니다. 새 문법을 넣거나 빼면 이 문서도 같이 고칩니다.
